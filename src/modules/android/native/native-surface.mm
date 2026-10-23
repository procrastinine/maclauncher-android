#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/CAMetalLayer.h>
#include <jni.h>
#include "native-import-registry.h"
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <cstdint>
#include <cinttypes>
#include <cerrno>
#include <dlfcn.h>
#include <cstring>
#include <vector>
#include <cmath>
#include "wide-strings.hpp"
#include "native-dlopen.hpp"
#include "surface-texture.hpp"
#ifdef KL_NATIVE_EGL_HOST
#include "presentation.h"
#include "pixel-copy.hpp"
#endif

namespace {

struct Window {uint64_t lease,consumer=0,geometryGeneration=1,eglGeneration=0;unsigned references=1,presentationCalls=0;bool bound=true,texture=false,nativeLayerStarting=false,consumerHeld=false;int width,height,format,defaultWidth,defaultHeight,defaultFormat,requestedWidth=0,requestedHeight=0,requestedFormat=0;IOSurfaceRef storage=nullptr,eglStorage=nullptr;size_t bytes=0;JavaVM *vm=nullptr;jobject composition=nullptr;int(*consumerRetain)(void*)=nullptr;void(*consumerRelease)(void*)=nullptr;void *presenter=nullptr,*egl=nullptr,*nativeLayer=nullptr;};
void *hostPresenter=nullptr;
std::mutex mutex;
std::unordered_set<Window*> windows;
uint64_t nextLease=1,allocations=0,retirements=0,externalGeneration=0;
size_t bytes=0;
struct StorageCharge {size_t references=0,bytes=0;};
std::unordered_map<IOSurfaceID,StorageCharge> storageCharges;
void chargeStorage(IOSurfaceRef storage,bool retain){
    if(!storage)return;auto id=IOSurfaceGetID(storage);
    if(retain){auto &charge=storageCharges[id];if(!charge.references++){charge.bytes=IOSurfaceGetAllocSize(storage);bytes+=charge.bytes;}}
    else{auto at=storageCharges.find(id);if(at!=storageCharges.end()&&!--at->second.references){bytes-=at->second.bytes;storageCharges.erase(at);}}
}
void releaseComposition(Window *value){
    if(!value->composition)return;JNIEnv *env=nullptr;bool attached=false;
    if(value->vm->GetEnv((void**)&env,JNI_VERSION_1_8)==JNI_EDETACHED)attached=value->vm->AttachCurrentThreadAsDaemon((void**)&env,nullptr)==JNI_OK;
    if(env)env->DeleteGlobalRef(value->composition);if(attached)value->vm->DetachCurrentThread();
}
void fail(JNIEnv *env,const char *message){if(env->ExceptionCheck())return;auto type=env->FindClass("java/lang/IllegalStateException");if(type)env->ThrowNew(type,message);}
Window *find(uint64_t lease){for(auto value:windows)if(value&&value->lease==lease)return value;return nullptr;}
Window *known(void *pointer){for(auto value:windows)if(value==pointer)return value;return nullptr;}
bool dimensions(int width,int height,int format){return width>0&&height>0&&width<=8192&&height<=8192&&(format==1||format==2||format==4);}
IOSurfaceRef allocate(int width,int height,int format){@autoreleasepool{
    size_t element=format==4?2:4,row=IOSurfaceAlignProperty(kIOSurfaceBytesPerRow,size_t(width)*element);
    NSDictionary *properties=@{(__bridge NSString*)kIOSurfaceWidth:@(width),(__bridge NSString*)kIOSurfaceHeight:@(height),(__bridge NSString*)kIOSurfaceBytesPerElement:@(element),(__bridge NSString*)kIOSurfaceBytesPerRow:@(row),(__bridge NSString*)kIOSurfacePixelFormat:@(format==4?(uint32_t)'L565':(uint32_t)'RGBA')};
    return IOSurfaceCreate((__bridge CFDictionaryRef)properties);
}}
// The Java Buffer owns the initial Window reference. External native references
// retain that same Buffer once, so a native window survives Surface.release().
// Drop its handle only after unlocking: Buffer.close releases the initial ref.
struct WindowLock {
    std::unique_lock<std::mutex> lock{mutex};
    struct Retired {void(*release)(void*);uint64_t handle;};std::vector<Retired> retired;
    bool retain(Window *value){
        if(value->references==1&&value->consumer&&!value->consumerHeld){
            if(!value->consumerRetain||!value->consumerRetain((void*)(uintptr_t)value->consumer))return false;value->consumerHeld=true;
        }
        value->references++;return true;
    }
    void retire(Window *value){
        if(value->references==2&&value->consumerHeld){retired.push_back({value->consumerRelease,value->consumer});value->consumerHeld=false;}
        if(--value->references)return;windows.erase(value);if(value->storage){chargeStorage(value->storage,false);CFRelease(value->storage);}releaseComposition(value);delete value;retirements++;
    }
    ~WindowLock(){lock.unlock();for(auto entry:retired)entry.release((void*)(uintptr_t)entry.handle);}
};
void *fromSurface(JNIEnv *env,jobject surface){
    if(!env||!surface||env->ExceptionCheck())return nullptr;
    auto type=env->FindClass("android/view/Surface");if(!type)return nullptr;bool matches=env->IsInstanceOf(surface,type);env->DeleteLocalRef(type);if(!matches)return nullptr;
    auto adapter=env->FindClass("maclauncher/android/framework/HostSurface");if(!adapter)return nullptr;
    auto method=env->GetStaticMethodID(adapter,"nativeLease","(Landroid/view/Surface;)J");if(!method){env->DeleteLocalRef(adapter);return nullptr;}
    auto lease=env->CallStaticLongMethod(adapter,method,surface);env->DeleteLocalRef(adapter);if(env->ExceptionCheck())return nullptr;
    WindowLock lock;auto value=find(lease);if(!value||!value->bound||!lock.retain(value))return nullptr;return value;
}
void acquire(void *pointer){WindowLock lock;if(auto value=known(pointer))lock.retain(value);}
void release(void *pointer){WindowLock lock;if(auto value=known(pointer))lock.retire(value);}
jobject toSurface(JNIEnv *env,void *pointer){
    if(!env||env->ExceptionCheck())return nullptr;
    uint64_t consumer=0;{WindowLock lock;auto value=known(pointer);if(!value||!value->consumer||!lock.retain(value))return nullptr;consumer=value->consumer;}
    auto type=env->FindClass("maclauncher/android/framework/HostSurface");jobject result=nullptr;
    if(type){auto method=env->GetStaticMethodID(type,"fromNativeHandle","(J)Landroid/view/Surface;");if(method){result=env->CallStaticObjectMethod(type,method,(jlong)consumer);if(env->ExceptionCheck())result=nullptr;}env->DeleteLocalRef(type);}release(pointer);return result;
}
int width(void *pointer){WindowLock lock;auto value=known(pointer);return value?value->defaultWidth:0;}
int height(void *pointer){WindowLock lock;auto value=known(pointer);return value?value->defaultHeight:0;}
int format(void *pointer){WindowLock lock;auto value=known(pointer);return value?value->format:0;}
// AOSP Surface queries expose the consumer's default extent. Producer requests
// select buffer storage independently; zero dimensions restore those defaults.
// Keep both identities so repeated resolution selection cannot shrink the
// reported window, invalidate touch coordinates, or lose an explicit request.
int bufferGeometryLocked(Window *value,int w,int h,int f){
    if(!value||!value->bound)return -ENODEV;
    if(!dimensions(w,h,f))return -EINVAL;
    if(w==value->width&&h==value->height&&f==value->format)return 0;
    // A Canvas-only remote producer already owns gralloc storage. Its native
    // window identity needs no IOSurface until an EGL producer actually binds.
    if(value->composition&&!value->storage){value->width=w;value->height=h;value->format=f;return 0;}
    // The window identity outlives individual buffers. An active EGL producer
    // retains its previous IOSurface until its render thread rebinds; changing
    // the next buffer must not reject a live ANativeWindow reference.
    size_t needed=size_t(w)*h*4;
    auto storage=allocate(w,h,f);if(!storage)return -ENOMEM;
    needed=IOSurfaceGetAllocSize(storage);
    auto old=value->storage;chargeStorage(storage,true);chargeStorage(old,false);value->storage=storage;value->bytes=needed;value->width=w;value->height=h;value->format=f;++value->geometryGeneration;CFRelease(old);
    if(value->nativeLayer){auto layer=(__bridge CAMetalLayer*)value->nativeLayer;layer.bounds=CGRectMake(0,0,w,h);layer.drawableSize=CGSizeMake(w,h);}
    return 0;
}
int geometryLocked(Window *value,int w,int h,int f){
    if(!value||!value->bound)return -ENODEV;
    if(w<0||h<0||f<0||(!w!=!h))return -EINVAL;
    int result=bufferGeometryLocked(value,w?w:value->defaultWidth,h?h:value->defaultHeight,f?f:value->defaultFormat);
    if(!result){value->requestedWidth=w;value->requestedHeight=h;value->requestedFormat=f;}
    return result;
}
int defaultGeometryLocked(Window *value,int w,int h,int f){
    if(!value||!value->bound)return -ENODEV;
    if(!f)f=value->defaultFormat;
    if(!dimensions(w,h,f))return -EINVAL;
    int result=bufferGeometryLocked(value,value->requestedWidth?value->requestedWidth:w,value->requestedHeight?value->requestedHeight:h,value->requestedFormat?value->requestedFormat:f);
    if(!result){value->defaultWidth=w;value->defaultHeight=h;value->defaultFormat=f;}
    return result;
}
int geometry(void *pointer,int w,int h,int f){WindowLock lock;return geometryLocked(known(pointer),w,h,f);}
// Native API return values use Android errno numbers, independently of the
// host thread's errno (Darwin ENOSYS is 78; Android ENOSYS is 38).
constexpr int androidNotImplemented=38;
int lockBuffer(void *,void *,void *){return -androidNotImplemented;}
int postBuffer(void *){return -androidNotImplemented;}
// Every public native-window operation stays in the Window owner's domain.
// Unsupported producer/display/Parcel facilities report the native error; an
// opaque host Window must never reach Android's unrelated android::Surface.
int bufferTransform(void *pointer,int transform){
    WindowLock lock;auto value=known(pointer);if(!value||!value->bound||transform<0||(transform&~7))return -EINVAL;
    return transform==0?0:-androidNotImplemented;
}
int bufferDataSpace(void *pointer,int dataSpace){
    WindowLock lock;auto value=known(pointer);if(!value||!value->bound||dataSpace!=0)return -EINVAL;
    return 0;
}
int dataSpace(void *pointer){WindowLock lock;auto value=known(pointer);return value&&value->bound?0:-EINVAL;}
int frameRateStrategy(void *pointer,float rate,int8_t compatibility,int8_t strategy){
    WindowLock lock;auto value=known(pointer);if(!value||!value->bound||!std::isfinite(rate)||rate<0)return -EINVAL;
    if(rate==0)return 0;
    if(compatibility<0||compatibility>1||strategy<0||strategy>1)return -EINVAL;
    return -androidNotImplemented;
}
int frameRate(void *pointer,float rate,int8_t compatibility){return frameRateStrategy(pointer,rate,compatibility,0);}
int clearFrameRate(void *pointer){return frameRateStrategy(pointer,0,0,0);}
void tryAllocateBuffers(void *pointer){
    WindowLock lock;auto value=known(pointer);if(!value||!value->bound||value->storage||value->composition)return;
    // Preallocation is a best-effort hint. Composed producers own their own
    // buffers; ordinary windows keep their eagerly allocated IOSurface.
    auto storage=allocate(value->width,value->height,value->format);if(!storage)return;
    value->storage=storage;value->bytes=IOSurfaceGetAllocSize(storage);chargeStorage(storage,true);
}
int readWindowParcel(const void *parcel,void **output){
    if(!output)return -EINVAL;*output=nullptr;if(!parcel)return -EINVAL;
    return -androidNotImplemented;
}
int writeWindowParcel(void *pointer,void *parcel){
    WindowLock lock;auto value=known(pointer);if(!value||!value->bound||!parcel)return -EINVAL;
    return -androidNotImplemented;
}
std::mutex installMutex;
bool importsInstalled=false;
#ifdef KL_NATIVE_EGL_HOST
extern "C" void *klj_native_egl_lookup(const char *name);
extern "C" void *klj_native_egl_guest_lookup(const char *name);
extern "C" const char *klj_native_egl_install(void *runtime,void *(*resolve)(const char*));
// Driver callbacks borrow the same tracked ANativeWindow and presenter as EGL.
// The retained surface keeps this context alive; unregistration drains all
// in-flight frames before that final window reference is retired.
struct NativeLayerEnvironment {
    JavaVM *vm;JNIEnv *env=nullptr;bool attached=false;
    explicit NativeLayerEnvironment(JavaVM *value):vm(value){
        if(!vm)return;
        auto result=vm->GetEnv((void**)&env,JNI_VERSION_1_8);
        if(result==JNI_EDETACHED){attached=vm->AttachCurrentThreadAsDaemon((void**)&env,nullptr)==JNI_OK;if(!attached)env=nullptr;}
        else if(result!=JNI_OK)env=nullptr;
    }
    ~NativeLayerEnvironment(){if(attached){if(env->ExceptionCheck()){env->ExceptionDescribe();env->ExceptionClear();}vm->DetachCurrentThread();}}
};
int nativeLayerFrame(void *pointer,void *commands,void *image){
    void *presenter;JavaVM *vm;
    {WindowLock lock;auto value=known(pointer);if(!value||!value->presenter)return 0;if(!lock.retain(value))return 0;value->presentationCalls++;presenter=value->presenter;vm=value->vm;}
    NativeLayerEnvironment environment(vm);
    int result=environment.env?klj_metal_native_frame(environment.env,presenter,commands,image):4;
    {WindowLock lock;auto value=known(pointer);value->presentationCalls--;lock.retire(value);}
    return result==0;
}
int acquireNativeLayer(void *pointer,void **owner,void **layer){
    JavaVM *vm;int width,height;
    {WindowLock lock;auto value=known(pointer);if(!value||!value->bound||!value->presenter||value->eglStorage||value->nativeLayer||value->nativeLayerStarting)return 0;if(!lock.retain(value))return 0;value->presentationCalls++;value->nativeLayerStarting=true;vm=value->vm;width=value->width;height=value->height;}
    NativeLayerEnvironment environment(vm);
    void *carrier=environment.env?klj_metal_native_layer(environment.env,width,height,nativeLayerFrame,pointer):nullptr;
    {WindowLock lock;auto value=known(pointer);value->presentationCalls--;value->nativeLayerStarting=false;if(!carrier){lock.retire(value);return 0;}value->nativeLayer=carrier;auto native=(__bridge CAMetalLayer*)carrier;native.bounds=CGRectMake(0,0,value->width,value->height);native.drawableSize=CGSizeMake(value->width,value->height);externalGeneration++;}
    *owner=pointer;*layer=carrier;return 1;
}
void releaseNativeLayer(void *pointer){
    void *carrier;
    {WindowLock lock;auto value=known(pointer);if(!value)return;carrier=value->nativeLayer;}
    klj_metal_release_native_layer(carrier);
    {WindowLock lock;auto value=known(pointer);value->nativeLayer=nullptr;externalGeneration++;lock.retire(value);}
}
#endif
void *lookup(const char *name){
    struct Entry {const char *name;void *address;};
    const Entry entries[]={
        {"android_dlopen_ext",(void*)native_dlopen::open},{"dlerror",(void*)native_dlopen::error},
        {"wcsnlen",(void*)wide_strings::length},{"wcslcpy",(void*)wide_strings::copy},{"wcslcat",(void*)wide_strings::append},{"wcsstr",(void*)wide_strings::find},
        {"ANativeWindow_fromSurface",(void*)fromSurface},{"ANativeWindow_acquire",(void*)acquire},{"ANativeWindow_release",(void*)release},
        {"ANativeWindow_toSurface",(void*)toSurface},
        {"ANativeWindow_getWidth",(void*)width},{"ANativeWindow_getHeight",(void*)height},{"ANativeWindow_getFormat",(void*)format},
        {"ANativeWindow_setBuffersGeometry",(void*)geometry},{"ANativeWindow_lock",(void*)lockBuffer},{"ANativeWindow_unlockAndPost",(void*)postBuffer},
        {"ANativeWindow_setBuffersTransform",(void*)bufferTransform},{"ANativeWindow_setBuffersDataSpace",(void*)bufferDataSpace},
        {"ANativeWindow_getBuffersDataSpace",(void*)dataSpace},{"ANativeWindow_getBuffersDefaultDataSpace",(void*)dataSpace},
        {"ANativeWindow_setFrameRate",(void*)frameRate},{"ANativeWindow_setFrameRateWithChangeStrategy",(void*)frameRateStrategy},
        {"ANativeWindow_clearFrameRate",(void*)clearFrameRate},{"ANativeWindow_tryAllocateBuffers",(void*)tryAllocateBuffers},
        {"ANativeWindow_readFromParcel",(void*)readWindowParcel},{"ANativeWindow_writeToParcel",(void*)writeWindowParcel}};
    for(auto &entry:entries)if(!strcmp(name,entry.name))return entry.address;
#ifdef KL_NATIVE_EGL_HOST
    if(auto result=klj_native_egl_guest_lookup(name))return result;
#endif
    return nullptr;
}
}

extern "C" void klj_video_initialize(JNIEnv*);
extern "C" JNIEXPORT jlong JNICALL Java_maclauncher_android_bridge_NativeSurface_create(JNIEnv *env,jclass,jint width,jint height,jint format){
    if(!dimensions(width,height,format)){fail(env,"Native surface dimensions or format are unsupported");return 0;}
    WindowLock lock;
    size_t needed=size_t(width)*height*4;
    auto storage=allocate(width,height,format);if(!storage){fail(env,"Native IOSurface allocation failed");return 0;}
    needed=IOSurfaceGetAllocSize(storage);
    auto value=new Window;env->GetJavaVM(&value->vm);value->lease=nextLease++;value->width=value->defaultWidth=width;value->height=value->defaultHeight=height;value->format=value->defaultFormat=format;value->storage=storage;value->bytes=needed;value->presenter=hostPresenter;
    windows.insert(value);chargeStorage(storage,true);allocations++;return value->lease;
}
extern "C" JNIEXPORT jlong JNICALL Java_maclauncher_android_bridge_NativeSurface_createComposition(JNIEnv *env,jclass,jint width,jint height,jint format,jobject producer){
    if(width<1||height<1||format!=1||!producer){fail(env,"Native composition surface descriptor is invalid");return 0;}
    auto reference=env->NewGlobalRef(producer);if(!reference)return 0;
    WindowLock lock;auto value=new Window;env->GetJavaVM(&value->vm);value->composition=reference;value->lease=nextLease++;value->width=value->defaultWidth=width;value->height=value->defaultHeight=height;value->format=value->defaultFormat=format;
    windows.insert(value);allocations++;return value->lease;
}
extern "C" jobject klj_surface_composition(JNIEnv *env,void *pointer){WindowLock lock;auto value=known(pointer);return value&&value->composition?env->NewLocalRef(value->composition):nullptr;}
extern "C" JNIEXPORT jboolean JNICALL Java_maclauncher_android_bridge_NativeSurface_valid(JNIEnv*,jclass,jlong lease){WindowLock lock;auto value=find(lease);return value&&value->bound;}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NativeSurface_bindConsumer(JNIEnv*env,jclass,jlong lease,jlong handle,jlong retain,jlong retire){klj_video_initialize(env);if(env->ExceptionCheck())return;WindowLock lock;auto value=find(lease);if(value&&value->bound){value->consumer=(uint64_t)handle;value->consumerRetain=(int(*)(void*))(uintptr_t)retain;value->consumerRelease=(void(*)(void*))(uintptr_t)retire;}}
extern "C" uint64_t klj_surface_consumer(void*pointer){WindowLock lock;auto value=known(pointer);return value&&value->bound?value->consumer:0;}
extern "C" bool klj_surface_retain_window(void*pointer){WindowLock lock;auto value=known(pointer);if(!value||!value->bound)return false;if(!lock.retain(value))return 0;return true;}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NativeSurface_release(JNIEnv*,jclass,jlong lease){WindowLock lock;auto value=find(lease);if(value&&value->bound){value->bound=false;lock.retire(value);}}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NativeSurface_resize(JNIEnv *env,jclass,jlong lease,jint width,jint height,jint format){
    int result;{WindowLock lock;result=defaultGeometryLocked(find(lease),width,height,format);}
    if(result)fail(env,"Native surface resize failed");
}
extern "C" JNIEXPORT jlongArray JNICALL Java_maclauncher_android_bridge_NativeSurface_statistics(JNIEnv *env,jclass){
    WindowLock lock;jlong live=0,refs=0;for(auto value:windows)if(value){live++;refs+=value->references;}
    jlong values[]={live,(jlong)bytes,refs,(jlong)allocations,(jlong)retirements};auto out=env->NewLongArray(5);if(out)env->SetLongArrayRegion(out,0,5,values);return out;
}
extern "C" JNIEXPORT jlong JNICALL Java_maclauncher_android_bridge_NativeSurface_acquire(JNIEnv *env,jclass,jobject surface){return (jlong)(uintptr_t)fromSurface(env,surface);}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NativeSurface_releaseWindow(JNIEnv*,jclass,jlong window){release((void*)(uintptr_t)window);}
extern "C" JNIEXPORT jintArray JNICALL Java_maclauncher_android_bridge_NativeSurface_geometry(JNIEnv *env,jclass,jlong window){
    WindowLock lock;auto value=known((void*)(uintptr_t)window);if(!value){fail(env,"Native window is not owned");return nullptr;}
    jint values[]={value->width,value->height,value->format};auto out=env->NewIntArray(3);if(out)env->SetIntArrayRegion(out,0,3,values);return out;
}
extern "C" JNIEXPORT jintArray JNICALL Java_maclauncher_android_bridge_NativeSurface_leaseGeometry(JNIEnv *env,jclass,jlong lease){
    WindowLock lock;auto value=find(lease);if(!value||!value->bound){fail(env,"Native surface lease is unavailable");return nullptr;}
    jint values[]={value->width,value->height,value->format};auto out=env->NewIntArray(3);if(out)env->SetIntArrayRegion(out,0,3,values);return out;
}
extern "C" JNIEXPORT jlong JNICALL Java_maclauncher_android_bridge_NativeSurface_storageId(JNIEnv*,jclass,jlong window){WindowLock lock;auto value=known((void*)(uintptr_t)window);return value&&value->storage?IOSurfaceGetID(value->storage):0;}
extern "C" JNIEXPORT jboolean JNICALL Java_maclauncher_android_bridge_NativeSurface_storageAlive(JNIEnv*,jclass,jlong id){auto value=IOSurfaceLookup((IOSurfaceID)id);if(!value)return false;CFRelease(value);return true;}
#ifdef KL_NATIVE_EGL_HOST
extern "C" JNIEXPORT jlong JNICALL Java_maclauncher_android_bridge_NativeSurface_requestPixelCopyRegion(JNIEnv*,jclass,jobject,jint,jint,jint,jint,jint,jint,jobject);
extern "C" JNIEXPORT jlong JNICALL Java_maclauncher_android_bridge_NativeSurface_requestPixelCopy(JNIEnv *env,jclass,jobject surface,jint width,jint height,jobject callback){
    return Java_maclauncher_android_bridge_NativeSurface_requestPixelCopyRegion(env,nullptr,surface,0,0,0,0,width,height,callback);
}
extern "C" JNIEXPORT jlong JNICALL Java_maclauncher_android_bridge_NativeSurface_requestPixelCopyRegion(JNIEnv *env,jclass,jobject surface,jint left,jint top,jint right,jint bottom,jint width,jint height,jobject callback){
    if(!callback){auto type=env->FindClass("java/lang/NullPointerException");if(type)env->ThrowNew(type,"PixelCopy callback");return 0;}
    auto pointer=fromSurface(env,surface);if(env->ExceptionCheck())return 0;
    if(!pointer){klj_copy::Queue unavailable;return unavailable.add(env,callback,width,height,klj_copy::SourceInvalid);}
    void *presenter=nullptr;IOSurfaceRef storage=nullptr;
    {WindowLock lock;auto value=known(pointer);if(value){value->presentationCalls++;presenter=value->presenter;auto image=value->eglStorage?value->eglStorage:value->storage;if(image)storage=(IOSurfaceRef)CFRetain(image);}}
    // The active enqueue borrows the presenter separately from buffer storage.
    // Its submitted GPU work is drained by the presenter at destruction.
    uint64_t id=klj_metal_copy_region(env,presenter,storage,left,top,right,bottom,width,height,callback);
    if(storage)CFRelease(storage);
    {WindowLock lock;auto value=known(pointer);if(value){value->presentationCalls--;lock.retire(value);}}return id;
}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NativeSurface_cancelPixelCopy(JNIEnv*,jclass,jlong id){klj_copy::cancel(id);}
#endif
extern "C" const char *klj_surface_install(const char *path){
    std::lock_guard installLock(installMutex);if(importsInstalled)return nullptr;
    void *library=dlopen(path,RTLD_NOW|RTLD_GLOBAL);
    if(!library)return dlerror();auto install=(int(*)(KljImportResolver))dlsym(library,"klj_register_import_resolver");if(!install)return "Native loader does not expose its host symbol registry";
    auto resolve=(void*(*)(const char*))dlsym(library,"kl_shim_lookup");if(!resolve)return "Native loader lacks its symbol resolver";
native_dlopen::guest_open=(decltype(native_dlopen::guest_open))resolve("dlopen");
    native_dlopen::guest_error=(decltype(native_dlopen::guest_error))resolve("dlerror");
    if(!native_dlopen::guest_open||!native_dlopen::guest_error)return "Native loader lacks its dynamic library adapter";
#ifdef KL_NATIVE_EGL_HOST
    if(auto error=klj_native_egl_install(library,resolve))return error;
    if(auto graphicsError=klj_native_egl_lookup("dlerror"))native_dlopen::guest_error=(decltype(native_dlopen::guest_error))graphicsError;
    auto vulkan=(int(*)(int(*)(void*,void**,void**),void(*)(void*)))dlsym(library,"klj_vulkan_set_window_bridge");
    if(!vulkan||!vulkan(acquireNativeLayer,releaseNativeLayer))return "Native Vulkan window registration failed";
#endif
    if(!install(lookup))return "Native import registration allocation failed";importsInstalled=true;return nullptr;
}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NativeSurface_install(JNIEnv *env,jclass,jstring runtime){
    const char *path=env->GetStringUTFChars(runtime,nullptr);if(!path)return;const char *error=klj_surface_install(path);env->ReleaseStringUTFChars(runtime,path);if(error)fail(env,error);
}
#ifdef KL_NATIVE_EGL_HOST
// Both a native Vulkan producer and ANGLE's Vulkan EGL backend borrow this
// same retained layer/window lease. The driver owns its images and commands.
extern "C" int klj_surface_acquire_native_layer(void *window,void **owner,void **layer){return acquireNativeLayer(window,owner,layer);}
extern "C" void klj_surface_release_native_layer(void *owner){releaseNativeLayer(owner);}
#endif
// Native EGL ownership uses the same tracked window as Java/ANativeWindow.
extern "C" void klj_surface_host(void *presenter){WindowLock lock;hostPresenter=presenter;}
extern "C" void *klj_surface_begin_egl(void *pointer,void **presenter,int *width,int *height,int format){
    WindowLock lock;auto value=known(pointer);if(!value||!value->bound||value->egl||value->eglStorage||value->nativeLayer||value->nativeLayerStarting||(!value->presenter&&!value->texture&&!value->composition))return nullptr;
    if(value->composition){if(format&&format!=1&&format!=2)return nullptr;format=1;}
    if(format){if(bufferGeometryLocked(value,value->width,value->height,format))return nullptr;value->requestedFormat=format;}
    if(!value->storage){value->storage=allocate(value->width,value->height,value->format);if(!value->storage)return nullptr;value->bytes=IOSurfaceGetAllocSize(value->storage);chargeStorage(value->storage,true);}
    if(!lock.retain(value))return 0;value->eglStorage=(IOSurfaceRef)CFRetain(value->storage);chargeStorage(value->eglStorage,true);value->eglGeneration=value->geometryGeneration;*presenter=value->presenter;*width=value->width;*height=value->height;return (void*)CFRetain(value->storage);
}
extern "C" void klj_surface_record_egl(void *pointer,void *egl){WindowLock lock;if(auto value=known(pointer)){if(value->egl!=egl)externalGeneration++;value->egl=egl;}}
extern "C" void *klj_surface_find_egl(void *egl,void **presenter){WindowLock lock;for(auto value:windows)if(value&&value->egl==egl&&egl){*presenter=value->presenter;return value;}return nullptr;}
extern "C" void klj_surface_end_egl(void *pointer){WindowLock lock;if(auto value=known(pointer)){if(value->egl)externalGeneration++;value->egl=nullptr;if(value->eglStorage){chargeStorage(value->eglStorage,false);CFRelease(value->eglStorage);value->eglStorage=nullptr;}lock.retire(value);}}
extern "C" void *klj_surface_native_window(JNIEnv *env,jobject surface){return fromSurface(env,surface);}
extern "C" void *klj_surface_acquire_lease(uint64_t lease,void **presenter,int *width,int *height){
    if(!presenter||!width||!height)return nullptr;
    WindowLock lock;auto value=find(lease);if(!value||!value->bound)return nullptr;
    if(!lock.retain(value))return 0;value->presentationCalls++;*presenter=value->presenter;*width=value->width;*height=value->height;return value;
}
extern "C" void klj_surface_release_lease(void *window){WindowLock lock;if(auto value=known(window)){value->presentationCalls--;lock.retire(value);}}
extern "C" void klj_surface_release_window(void *window){release(window);}
extern "C" bool klj_surface_detach_presenter(void *presenter){
    if(!presenter)return true;WindowLock lock;
    // Buffer references do not own the display. Only an active producer or a
    // call currently submitting work can retain its live presenter.
    for(auto value:windows)if(value->presenter==presenter&&(value->eglStorage||value->nativeLayer||value->presentationCalls))return false;
    if(hostPresenter==presenter)hostPresenter=nullptr;
    for(auto value:windows)if(value->presenter==presenter)value->presenter=nullptr;
    return true;
}
extern "C" bool klj_surface_texture_owner(uint64_t lease){WindowLock lock;auto value=find(lease);if(!value||!value->bound||value->egl||value->nativeLayer||value->nativeLayerStarting)return false;value->texture=true;value->presenter=nullptr;return true;}
extern "C" bool klj_surface_is_texture(void *pointer){WindowLock lock;auto value=known(pointer);return value&&value->texture;}
extern "C" uint64_t klj_surface_lease(void *pointer){WindowLock lock;auto value=known(pointer);return value?value->lease:0;}
extern "C" void *klj_surface_copy_storage(void *pointer){WindowLock lock;auto value=known(pointer);auto image=value?(value->eglStorage?value->eglStorage:value->storage):nullptr;return image?(void*)CFRetain(image):nullptr;}

// Buffer adoption happens on the EGL producer after old GPU work is retired.
extern "C" void *klj_surface_next_egl(void *pointer,int *width,int *height){
 WindowLock lock;auto value=known(pointer);if(!value||!value->eglStorage||value->geometryGeneration==value->eglGeneration)return nullptr;
 *width=value->width;*height=value->height;return (void*)CFRetain(value->storage);
}
extern "C" void klj_surface_commit_egl(void *pointer,void *storage){
 WindowLock lock;auto value=known(pointer);if(!value||!value->eglStorage||!storage)return;
 auto old=value->eglStorage;value->eglStorage=(IOSurfaceRef)CFRetain(storage);chargeStorage(value->eglStorage,true);chargeStorage(old,false);if(storage==value->storage)value->eglGeneration=value->geometryGeneration;CFRelease(old);
}
extern "C" void klj_surface_rotate_egl(void *pointer,void *storage){
 WindowLock lock;auto value=known(pointer);if(!value||!value->eglStorage||!storage)return;
 auto old=value->eglStorage;value->eglStorage=(IOSurfaceRef)CFRetain(storage);chargeStorage(value->eglStorage,true);chargeStorage(old,false);CFRelease(old);
}
extern "C" void klj_surface_account_egl_buffer(void *storage,bool retain){WindowLock lock;chargeStorage(static_cast<IOSurfaceRef>(storage),retain);}
extern "C" JNIEXPORT jboolean JNICALL Java_maclauncher_android_bridge_NativeSurface_externallyPresented(JNIEnv*,jclass,jlong lease){WindowLock lock;auto value=find(lease);return value&&value->presenter&&(value->egl||value->nativeLayer);}
extern "C" JNIEXPORT jlong JNICALL Java_maclauncher_android_bridge_NativeSurface_externalGeneration(JNIEnv*,jclass){WindowLock lock;return externalGeneration;}
