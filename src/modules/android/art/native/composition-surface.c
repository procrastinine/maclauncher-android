// Public NDK physical boundary. SurfaceFlinger owns these independent layers,
// buffers and release fences; the Mac output remains the existing presenter.
#include <android/hardware_buffer.h>
#include <android/surface_control.h>
#include <android/surface_control_jni.h>
#include <jni.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static void fail(JNIEnv* env,const char* message){
    jclass type=(*env)->FindClass(env,"java/lang/IllegalStateException");
    if(type)(*env)->ThrowNew(env,type,message);
}
static ASurfaceControl* control(jlong value){return (ASurfaceControl*)(uintptr_t)value;}
static AHardwareBuffer* buffer(jlong value){return (AHardwareBuffer*)(uintptr_t)value;}

JNIEXPORT jlong JNICALL Java_maclauncher_android_bridge_NativeComposition_rootNative(JNIEnv* env,jclass type,jobject parent){
    (void)type;ASurfaceControl* value=ASurfaceControl_fromJava(env,parent);
    if(!value&&!(*env)->ExceptionCheck(env))fail(env,"Original SurfaceControl is unavailable");
    return (jlong)(uintptr_t)value;
}
JNIEXPORT jlong JNICALL Java_maclauncher_android_bridge_NativeComposition_layerNative(JNIEnv* env,jclass type,jlong parent,jstring name){
    (void)type;const char* text=(*env)->GetStringUTFChars(env,name,NULL);if(!text)return 0;
    ASurfaceControl* value=ASurfaceControl_create(control(parent),text);
    (*env)->ReleaseStringUTFChars(env,name,text);
    if(!value)fail(env,"Original SurfaceControl allocation failed");return (jlong)(uintptr_t)value;
}
JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NativeComposition_releaseControlNative(JNIEnv* env,jclass type,jlong pointer,jboolean detach){
    (void)env;(void)type;if(!pointer)return;
    if(detach){ASurfaceTransaction* transaction=ASurfaceTransaction_create();ASurfaceTransaction_reparent(transaction,control(pointer),NULL);ASurfaceTransaction_apply(transaction);ASurfaceTransaction_delete(transaction);}
    ASurfaceControl_release(control(pointer));
}
JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NativeComposition_stateNative(JNIEnv* env,jclass type,jlong pointer,jfloat x,jfloat y,jint z,jfloat alpha,jboolean visible){
    (void)env;(void)type;ASurfaceTransaction* transaction=ASurfaceTransaction_create();
    ASurfaceTransaction_setPosition(transaction,control(pointer),x,y);
    ASurfaceTransaction_setZOrder(transaction,control(pointer),z);
    ASurfaceTransaction_setBufferAlpha(transaction,control(pointer),alpha);
    ASurfaceTransaction_setVisibility(transaction,control(pointer),visible?ASURFACE_TRANSACTION_VISIBILITY_SHOW:ASURFACE_TRANSACTION_VISIBILITY_HIDE);
    ASurfaceTransaction_apply(transaction);ASurfaceTransaction_delete(transaction);
}
JNIEXPORT jlong JNICALL Java_maclauncher_android_bridge_NativeComposition_bufferNative(JNIEnv* env,jclass type,jint width,jint height){
    (void)type;AHardwareBuffer_Desc descriptor={0};descriptor.width=(uint32_t)width;descriptor.height=(uint32_t)height;descriptor.layers=1;
    descriptor.format=AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    descriptor.usage=AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN|AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE|AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
    AHardwareBuffer* value=NULL;int status=AHardwareBuffer_allocate(&descriptor,&value);
    if(status||!value){fail(env,"Original hardware buffer allocation failed");return 0;}
    return (jlong)(uintptr_t)value;
}
JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NativeComposition_releaseBufferNative(JNIEnv* env,jclass type,jlong pointer){
    (void)env;(void)type;if(pointer)AHardwareBuffer_release(buffer(pointer));
}
JNIEXPORT jintArray JNICALL Java_maclauncher_android_bridge_NativeComposition_descriptionNative(JNIEnv* env,jclass type,jlong pointer){
    (void)type;AHardwareBuffer_Desc descriptor={0};AHardwareBuffer_describe(buffer(pointer),&descriptor);
    jint values[]={(jint)descriptor.width,(jint)descriptor.height,(jint)descriptor.stride,(jint)descriptor.format};
    jintArray result=(*env)->NewIntArray(env,4);if(result)(*env)->SetIntArrayRegion(env,result,0,4,values);return result;
}
// The pinned physical allocator supplies one coherent RGBA memfd, followed by
// its metadata. Export through the original public AHardwareBuffer transport;
// do not recreate a native_handle or copy its pixel storage. Only this physical
// supplier's single-plane storage is exposed by the CPU producer boundary.
JNIEXPORT jint JNICALL Java_maclauncher_android_bridge_NativeComposition_storageNative(JNIEnv* env,jclass type,jlong pointer){
    (void)type;int sockets[2]={-1,-1},result=-1,fds[16];size_t count=0;
    if(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,sockets))goto done;
    if(AHardwareBuffer_sendHandleToUnixSocket(buffer(pointer),sockets[0]))goto done;
    char bytes[4096],ancillary[CMSG_SPACE(sizeof fds)];struct iovec vector={bytes,sizeof bytes};
    struct msghdr message={0};message.msg_iov=&vector;message.msg_iovlen=1;message.msg_control=ancillary;message.msg_controllen=sizeof ancillary;
    ssize_t received;do{received=recvmsg(sockets[1],&message,MSG_CMSG_CLOEXEC);}while(received<0&&errno==EINTR);
    if(received<=0)goto done;
    for(struct cmsghdr* item=CMSG_FIRSTHDR(&message);item;item=CMSG_NXTHDR(&message,item)){
        if(item->cmsg_level!=SOL_SOCKET||item->cmsg_type!=SCM_RIGHTS)continue;
        size_t n=(item->cmsg_len-CMSG_LEN(0))/sizeof(int);const int* values=(const int*)CMSG_DATA(item);
        for(size_t i=0;i<n;i++){if(count<16)fds[count++]=values[i];else close(values[i]);}
    }
    if(!(message.msg_flags&(MSG_TRUNC|MSG_CTRUNC))&&count==1){
        AHardwareBuffer_Desc descriptor={0};AHardwareBuffer_describe(buffer(pointer),&descriptor);struct stat info;
        uint64_t needed=(uint64_t)descriptor.stride*descriptor.height*4;
        if(descriptor.format==AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM&&descriptor.layers==1&&
           !fstat(fds[0],&info)&&info.st_size>=0&&needed<=(uint64_t)info.st_size){result=fds[0];fds[0]=-1;}
    }
done:
    for(size_t i=0;i<count;i++)if(fds[i]>=0)close(fds[i]);
    for(unsigned i=0;i<2;i++)if(sockets[i]>=0)close(sockets[i]);
    if(result<0)fail(env,"Original hardware buffer storage cannot be exported");return result;
}

struct Release {JavaVM* vm;jobject callback;jmethodID method;jlong token;};
static void released(void* opaque,int fence){
    struct Release* value=opaque;JNIEnv* env=NULL;int attached=0;
    jint status=(*value->vm)->GetEnv(value->vm,(void**)&env,JNI_VERSION_1_6);
    if(status==JNI_EDETACHED){attached=(*value->vm)->AttachCurrentThreadAsDaemon(value->vm,&env,NULL)==JNI_OK;if(!attached)env=NULL;}
    else if(status!=JNI_OK)env=NULL;
    if(env){(*env)->CallVoidMethod(env,value->callback,value->method,value->token,(jint)fence);(*env)->DeleteGlobalRef(env,value->callback);
        if((*env)->ExceptionCheck(env)){(*env)->ExceptionDescribe(env);(*env)->ExceptionClear(env);}}
    else if(fence>=0)close(fence);
    JavaVM* vm=value->vm;free(value);if(attached)(*vm)->DetachCurrentThread(vm);
}
static void committed(void* opaque,ASurfaceTransactionStats* stats){
    struct Release* value=opaque;JNIEnv* env=NULL;int attached=0;
    jint status=(*value->vm)->GetEnv(value->vm,(void**)&env,JNI_VERSION_1_6);
    if(status==JNI_EDETACHED){attached=(*value->vm)->AttachCurrentThreadAsDaemon(value->vm,&env,NULL)==JNI_OK;if(!attached)env=NULL;}
    else if(status!=JNI_OK)env=NULL;
    if(env){(*env)->CallVoidMethod(env,value->callback,value->method,value->token,(jlong)ASurfaceTransactionStats_getLatchTime(stats));(*env)->DeleteGlobalRef(env,value->callback);
        if((*env)->ExceptionCheck(env)){(*env)->ExceptionDescribe(env);(*env)->ExceptionClear(env);}}
    JavaVM* vm=value->vm;free(value);if(attached)(*vm)->DetachCurrentThread(vm);
}
JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NativeComposition_barrierNative(JNIEnv* env,jclass type,jlong root,jobject callback,jlong frame){
    (void)type;struct Release* value=calloc(1,sizeof(*value));if(!value){fail(env,"Original surface completion allocation failed");return;}
    (*env)->GetJavaVM(env,&value->vm);value->callback=(*env)->NewGlobalRef(env,callback);value->token=frame;
    jclass owner=(*env)->GetObjectClass(env,callback);value->method=(*env)->GetMethodID(env,owner,"latched","(JJ)V");(*env)->DeleteLocalRef(env,owner);
    if(!value->callback||!value->method){if(value->callback)(*env)->DeleteGlobalRef(env,value->callback);free(value);return;}
    ASurfaceTransaction* transaction=ASurfaceTransaction_create();ASurfaceTransaction_setPosition(transaction,control(root),0,0);ASurfaceTransaction_setOnCommit(transaction,value,committed);ASurfaceTransaction_apply(transaction);ASurfaceTransaction_delete(transaction);
}
JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NativeComposition_submitNative(JNIEnv* env,jclass type,jlong layer,jlong storage,jint fence,jobject callback,jlong token,jlong frame){
    (void)type;struct Release* value=calloc(1,sizeof(*value));
    struct Release* latch=calloc(1,sizeof(*latch));
    if(!value||!latch){free(value);free(latch);if(fence>=0)close(fence);fail(env,"Original surface callback allocation failed");return;}
    (*env)->GetJavaVM(env,&value->vm);value->callback=(*env)->NewGlobalRef(env,callback);value->token=token;
    latch->vm=value->vm;latch->callback=(*env)->NewGlobalRef(env,callback);latch->token=frame;
    jclass owner=(*env)->GetObjectClass(env,callback);value->method=(*env)->GetMethodID(env,owner,"released","(JI)V");latch->method=(*env)->GetMethodID(env,owner,"latched","(JJ)V");(*env)->DeleteLocalRef(env,owner);
    if(!value->callback||!value->method||!latch->callback||!latch->method){if(fence>=0)close(fence);if(value->callback)(*env)->DeleteGlobalRef(env,value->callback);if(latch->callback)(*env)->DeleteGlobalRef(env,latch->callback);free(value);free(latch);return;}
    ASurfaceTransaction* transaction=ASurfaceTransaction_create();
    ASurfaceTransaction_setBufferWithRelease(transaction,control(layer),buffer(storage),fence,value,released);
    ASurfaceTransaction_setOnCommit(transaction,latch,committed);
    ASurfaceTransaction_apply(transaction);ASurfaceTransaction_delete(transaction);
}
