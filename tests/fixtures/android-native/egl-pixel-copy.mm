#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <IOSurface/IOSurface.h>
#include <jni.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <string>
#include "presentation.h"

static void require(bool okay,const char *message){if(!okay)throw std::runtime_error(message);}
static void *library;
template<class T>static T exported(const char *name){auto value=dlsym(library,name);require(value,name);return reinterpret_cast<T>(value);}
static void *(*lookup)(const char*);
template<class T>static T api(const char *name){auto value=lookup(name);require(value,name);return reinterpret_cast<T>(value);}
static void java(JNIEnv *env){if(env->ExceptionCheck()){env->ExceptionDescribe();throw std::runtime_error("Original JNI callback failed");}}

int main(int argc,char **argv){@autoreleasepool{try{
    require(argc==4,"Expected graphics library, ANGLE directory and Java classes");
    [NSApplication sharedApplication];
    auto window=[[NSWindow alloc]initWithContentRect:NSMakeRect(0,0,128,128) styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
    window.releasedWhenClosed=NO;window.level=NSNormalWindowLevel-1;window.contentView.wantsLayer=YES;
    auto layer=[CAMetalLayer layer];layer.frame=CGRectMake(0,0,128,128);[window.contentView.layer addSublayer:layer];[window orderBack:nil];[window display];
    JavaVM *vm=nullptr;JNIEnv *env=nullptr;
    std::string classpath="-Djava.class.path="+std::string(argv[3]);JavaVMOption option{classpath.data(),nullptr};
    JavaVMInitArgs arguments{JNI_VERSION_1_8,1,&option,JNI_FALSE};
    require(JNI_CreateJavaVM(&vm,reinterpret_cast<void**>(&env),&arguments)==JNI_OK,"Cannot create original Java VM");
    setenv("KL_ANGLE_DIR",argv[2],1);setenv("KL_ANGLE_BACKEND","metal",1);
    library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!library)throw std::runtime_error(dlerror());
    exported<void*>("klj_metal_present_retained");
    exported<jint(*)(JavaVM*,void*)>("JNI_OnLoad")(vm,nullptr);
    auto file=env->NewStringUTF(argv[1]);exported<void(*)(JNIEnv*,jclass,jstring)>("Java_maclauncher_android_bridge_NativeSurface_configureGraphics")(env,nullptr,file);env->DeleteLocalRef(file);java(env);
    lookup=exported<decltype(lookup)>("klj_native_egl_lookup");
    auto presenter=exported<void*(*)(JNIEnv*,void*,int,int)>("klj_metal_create")(env,(__bridge void*)layer,32,32);java(env);require(presenter,"No Metal presenter");
    exported<void(*)(void*)>("klj_surface_host")(presenter);
    auto lease=exported<jlong(*)(JNIEnv*,jclass,int,int,int)>("Java_maclauncher_android_bridge_NativeSurface_create")(env,nullptr,32,32,1);java(env);
    void *actual=nullptr;int width=0,height=0;
    auto nativeWindow=exported<void*(*)(uint64_t,void**,int*,int*)>("klj_surface_acquire_lease")(lease,&actual,&width,&height);require(nativeWindow&&actual==presenter,"Native window owner differs");
    auto display=api<decltype(&eglGetDisplay)>("eglGetDisplay")(EGL_DEFAULT_DISPLAY);require(display,"No ANGLE display");
    require(api<decltype(&eglInitialize)>("eglInitialize")(display,nullptr,nullptr),"ANGLE initialize failed");
    EGLConfig config=nullptr;int found=0;const int configAttributes[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
    require(api<decltype(&eglChooseConfig)>("eglChooseConfig")(display,configAttributes,&config,1,&found)&&found,"No original EGL config");
    const int contextAttributes[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
    auto context=api<decltype(&eglCreateContext)>("eglCreateContext")(display,config,EGL_NO_CONTEXT,contextAttributes);require(context,"No original EGL context");
    auto surface=api<decltype(&eglCreateWindowSurface)>("eglCreateWindowSurface")(display,config,(EGLNativeWindowType)nativeWindow,nullptr);java(env);require(surface,"No EGL window surface");
    require(api<decltype(&eglMakeCurrent)>("eglMakeCurrent")(display,surface,surface,context),"Cannot bind EGL window");java(env);
    require(!api<decltype(&eglBindTexImage)>("eglBindTexImage")(display,surface,EGL_BACK_BUFFER),"EGL window exposed a writable texture alias");
    require(api<decltype(&eglGetError)>("eglGetError")()==EGL_BAD_SURFACE,"Original window texture-binding error differs");
    require(!api<decltype(&eglReleaseTexImage)>("eglReleaseTexImage")(display,surface,EGL_BACK_BUFFER),"EGL window accepted texture release");
    require(api<decltype(&eglGetError)>("eglGetError")()==EGL_BAD_SURFACE,"Original window texture-release error differs");
    auto clearColor=api<decltype(&glClearColor)>("glClearColor");auto clear=api<decltype(&glClear)>("glClear");
    auto swap=api<decltype(&eglSwapBuffers)>("eglSwapBuffers");
    auto provider=exported<void*(*)(const char*)>("klj_graphics_symbol");
    auto rawCurrent=reinterpret_cast<decltype(&eglGetCurrentSurface)>(provider("eglGetCurrentSurface"));
    std::set<void*> backings;int behavior=0;
    require(api<decltype(&eglQuerySurface)>("eglQuerySurface")(display,surface,EGL_SWAP_BEHAVIOR,&behavior)&&behavior==EGL_BUFFER_DESTROYED,"Window swap behavior changed");
    for(unsigned frame=0;frame<12;++frame){
        backings.insert(rawCurrent(EGL_DRAW));clearColor(frame%2?1:0,0,frame%2?0:1,1);clear(GL_COLOR_BUFFER_BIT);
        require(swap(display,surface),"Rotating original EGL swap failed");java(env);
        require(api<decltype(&eglGetCurrentSurface)>("eglGetCurrentSurface")(EGL_DRAW)==surface,"Public EGL identity changed");
    }
    require(backings.size()>1&&backings.size()<=3,"EGL image pool did not rotate within three buffers");
    // The next producer writes its newly dequeued buffer without submitting
    // it. An idle read must still observe the preceding submitted red frame.
    clearColor(0,0,1,1);clear(GL_COLOR_BUFFER_BIT);api<decltype(&glFinish)>("glFinish")();
    auto callbackType=env->FindClass("EglPixelCopyProbe");java(env);require(callbackType,"Missing callback observer");
    auto constructor=env->GetMethodID(callbackType,"<init>","()V"),await=env->GetMethodID(callbackType,"await","(I)V");java(env);
    auto copy=[&](unsigned color){
        auto callback=env->NewObject(callbackType,constructor);java(env);
        auto storage=exported<void*(*)(void*)>("klj_surface_copy_storage")(nativeWindow);require(storage,"No native producer buffer");
        auto token=exported<uint64_t(*)(JNIEnv*,void*,void*,int,int,jobject)>("klj_metal_copy")(env,presenter,storage,2,2,callback);CFRelease(storage);java(env);require(token,"Idle copy was not admitted");
        env->CallVoidMethod(callback,await,(jint)color);java(env);env->DeleteLocalRef(callback);
    };
    copy(0xffff0000);
    // A refused producer handoff must not publish this still-writable buffer.
    auto storage=exported<void*(*)(void*)>("klj_surface_copy_storage")(nativeWindow);
    KLMetalBufferSwap refused{rawCurrent(EGL_DRAW),storage,nullptr,[](void*){return 0;},false};
    auto owner=new int(1);
    require(!exported<decltype(&klj_metal_present_retained)>("klj_metal_present_retained")(env,presenter,0,owner,[](void *value){delete static_cast<int*>(value);},&refused),"Refused EGL handoff succeeded");
    require(env->ExceptionCheck(),"Refused EGL handoff did not report failure");env->ExceptionClear();CFRelease(storage);
    clearColor(0,1,0,1);clear(GL_COLOR_BUFFER_BIT);api<decltype(&glFinish)>("glFinish")();copy(0xffff0000);
    require(swap(display,surface),"Second idle source swap failed");java(env);copy(0xff00ff00);
    // Rejected target timing still releases the old producer by rotating it.
    auto target=exported<unsigned(*)(void*,void*,void*,int64_t)>("klj_metal_target");
    require(target(presenter,display,rawCurrent(EGL_DRAW),(int64_t)((CACurrentMediaTime()+2)*1e9))==0,"Cannot set original presentation target");
    auto beforeRejected=rawCurrent(EGL_DRAW);clearColor(0,0,1,1);clear(GL_COLOR_BUFFER_BIT);
    require(!swap(display,surface),"Out-of-range presentation target was accepted");java(env);
    require(api<decltype(&eglGetError)>("eglGetError")()==EGL_BAD_ACCESS,"Rejected target error differs");
    require(rawCurrent(EGL_DRAW)!=beforeRejected,"Rejected target left the submitted buffer producer-current");
    clearColor(0,1,0,1);clear(GL_COLOR_BUFFER_BIT);api<decltype(&glFinish)>("glFinish")();copy(0xff0000ff);
    require(target(presenter,display,rawCurrent(EGL_DRAW),0)==0,"Cannot reset original presentation target");
    require(swap(display,surface),"EGL swap after target rejection failed");java(env);copy(0xff00ff00);
    int capabilities=0;require(api<decltype(&eglGetConfigAttrib)>("eglGetConfigAttrib")(display,config,EGL_SURFACE_TYPE,&capabilities),"No EGL config capabilities");
    if(capabilities&EGL_SWAP_BEHAVIOR_PRESERVED_BIT){
        require(api<decltype(&eglSurfaceAttrib)>("eglSurfaceAttrib")(display,surface,EGL_SWAP_BEHAVIOR,EGL_BUFFER_PRESERVED),"Original preserved swap was rejected");
        clearColor(0,1,0,1);clear(GL_COLOR_BUFFER_BIT);require(swap(display,surface),"Preserved EGL swap failed");java(env);
        unsigned char pixel[4]={};api<decltype(&glReadPixels)>("glReadPixels")(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);require(pixel[0]==0&&pixel[1]==255&&pixel[2]==0&&pixel[3]==255,"Preserved EGL contents were lost");copy(0xff00ff00);
    }else{
        require(!api<decltype(&eglSurfaceAttrib)>("eglSurfaceAttrib")(display,surface,EGL_SWAP_BEHAVIOR,EGL_BUFFER_PRESERVED),"Unsupported preserved behavior was accepted");
        require(api<decltype(&eglGetError)>("eglGetError")()==EGL_BAD_MATCH,"Original preserved-buffer error changed");
    }
    exported<void(*)(JNIEnv*,jclass,jlong,int,int,int)>("Java_maclauncher_android_bridge_NativeSurface_resize")(env,nullptr,lease,48,16,1);java(env);
    require(swap(display,surface),"EGL resize generation failed");java(env);
    int extent=0;require(api<decltype(&eglQuerySurface)>("eglQuerySurface")(display,surface,EGL_WIDTH,&extent)&&extent==48,"Resized EGL width differs");
    require(api<decltype(&eglQuerySurface)>("eglQuerySurface")(display,surface,EGL_HEIGHT,&extent)&&extent==16,"Resized EGL height differs");
    clearColor(1,0,0,1);clear(GL_COLOR_BUFFER_BIT);require(swap(display,surface),"Resized EGL source failed");java(env);copy(0xffff0000);
    require(api<decltype(&eglDestroySurface)>("eglDestroySurface")(display,surface),"EGL window retirement failed");java(env);
    require(api<decltype(&eglGetCurrentSurface)>("eglGetCurrentSurface")(EGL_DRAW)==surface,"Destroyed-current EGL surface lost its public identity");
    auto recreated=api<decltype(&eglCreateWindowSurface)>("eglCreateWindowSurface")(display,config,(EGLNativeWindowType)nativeWindow,nullptr);java(env);require(recreated&&recreated!=surface,"EGL window recreation failed");
    require(api<decltype(&eglGetCurrentSurface)>("eglGetCurrentSurface")(EGL_DRAW)==surface,"Recycled raw ID stole destroyed-current identity");
    require(!api<decltype(&eglQuerySurface)>("eglQuerySurface")(display,surface,EGL_WIDTH,&extent),"Destroyed EGL handle queried a newly allocated backing");
    require(api<decltype(&eglGetError)>("eglGetError")()==EGL_BAD_SURFACE,"Destroyed EGL handle query error differs");
    require(!swap(display,surface),"Destroyed EGL handle swapped a newly allocated backing");
    require(api<decltype(&eglGetError)>("eglGetError")()==EGL_BAD_SURFACE,"Destroyed EGL handle swap error differs");
    require(api<decltype(&eglMakeCurrent)>("eglMakeCurrent")(display,recreated,recreated,context),"Cannot bind recreated EGL window");java(env);
    surface=recreated;
    for(unsigned frame=0;frame<5;++frame){clearColor(0,1,0,1);clear(GL_COLOR_BUFFER_BIT);require(swap(display,surface),"Recreated EGL buffer membership was not retired");java(env);}
    copy(0xff00ff00);
    auto destroyedStorage=static_cast<IOSurfaceRef>(exported<void*(*)(void*)>("klj_surface_copy_storage")(nativeWindow));
    auto initialId=exported<jlong(*)(JNIEnv*,jclass,jlong)>("Java_maclauncher_android_bridge_NativeSurface_storageId")(env,nullptr,(jlong)(uintptr_t)nativeWindow);
    require(api<decltype(&eglDestroySurface)>("eglDestroySurface")(display,surface),"Recreated EGL window retirement failed");java(env);
    auto statistics=exported<jlongArray(*)(JNIEnv*,jclass)>("Java_maclauncher_android_bridge_NativeSurface_statistics");
    auto values=statistics(env,nullptr);jlong held[5];env->GetLongArrayRegion(values,0,5,held);env->DeleteLocalRef(values);
    require(held[1]>=(jlong)IOSurfaceGetAllocSize(destroyedStorage)*(initialId==IOSurfaceGetID(destroyedStorage)?1:2),"Destroyed-current image allocation charge retired before unbind");CFRelease(destroyedStorage);
    require(api<decltype(&eglMakeCurrent)>("eglMakeCurrent")(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT),"Cannot release EGL context");
    require(api<decltype(&eglDestroyContext)>("eglDestroyContext")(display,context),"EGL context retirement failed");
    require(api<decltype(&eglTerminate)>("eglTerminate")(display),"EGL display retirement failed");
    exported<void(*)(void*)>("klj_surface_release_lease")(nativeWindow);
    exported<void(*)(JNIEnv*,jclass,jlong)>("Java_maclauncher_android_bridge_NativeSurface_release")(env,nullptr,lease);
    exported<void(*)(void*)>("klj_metal_destroy")(presenter);[window close];
    bool retired=false;for(unsigned retry=0;retry<100&&!retired;++retry){auto values=statistics(env,nullptr);jlong counts[5];env->GetLongArrayRegion(values,0,5,counts);env->DeleteLocalRef(values);retired=counts[0]==0&&counts[1]==0&&counts[2]==0;if(!retired)CFRunLoopRunInMode(kCFRunLoopDefaultMode,.005,false);}
    require(retired,"EGL window/image allocation charges did not retire");
    puts("Original EGL: stable window, three-buffer reuse, quiescent PixelCopy, swap behavior and resize PASS");
    vm->DestroyJavaVM();return 0;
}catch(const std::exception &error){fprintf(stderr,"%s\n",error.what());return 1;}}}
