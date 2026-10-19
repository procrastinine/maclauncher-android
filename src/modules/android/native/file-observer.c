#include <jni.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <poll.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The original AOSP ObserverThread consumes the same real inotify descriptor
 * implementation as native Android clients. It owns callback dispatch and weak
 * observer references; this boundary owns only descriptor I/O and shutdown. */
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static int descriptor=-1;
static unsigned char pending[16384];
static size_t cursor,available;
static void fail(JNIEnv *env,const char *type,const char *message){jclass c=(*env)->FindClass(env,type);if(c){(*env)->ThrowNew(env,c,message);(*env)->DeleteLocalRef(env,c);}}
static int bytes(JNIEnv *env,jbyteArray input,char out[PATH_MAX]){
    if(!input){fail(env,"java/lang/NullPointerException","Observed path is null");return 0;}
    jsize n=(*env)->GetArrayLength(env,input);
    if(n<1||n>=PATH_MAX){fail(env,"java/lang/IllegalArgumentException","Observed path exceeds its limit");return 0;}
    (*env)->GetByteArrayRegion(env,input,0,n,(jbyte*)out);if((*env)->ExceptionCheck(env))return 0;
    if(memchr(out,0,n)){fail(env,"java/lang/IllegalArgumentException","Observed path contains NUL");return 0;}out[n]=0;return 1;
}
JNIEXPORT jint JNICALL Java_maclauncher_android_framework_HostFileObserver_nativeInit(JNIEnv *env,jclass cls){
    (void)env;(void)cls;pthread_mutex_lock(&gate);if(descriptor<0)descriptor=inotify_init1(IN_CLOEXEC|IN_NONBLOCK);int result=descriptor;pthread_mutex_unlock(&gate);return result;
}
JNIEXPORT jint JNICALL Java_maclauncher_android_framework_HostFileObserver_nativeStart(JNIEnv *env,jclass cls,jbyteArray rootBytes,jbyteArray pathBytes,jint mask){
    (void)cls;char root[PATH_MAX],input[PATH_MAX],path[PATH_MAX];
    if(!bytes(env,rootBytes,root)||!bytes(env,pathBytes,input))return -1;
    if(!realpath(input,path))return -1;
    size_t n=strlen(root);if(strncmp(root,path,n)||(path[n]!=0&&path[n]!='/')){fail(env,"java/lang/SecurityException","File observation requires application-private storage");return -1;}
    pthread_mutex_lock(&gate);int result=descriptor<0?-1:inotify_add_watch(descriptor,path,(uint32_t)mask);pthread_mutex_unlock(&gate);return result;
}
JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostFileObserver_nativeStop(JNIEnv *env,jclass cls,jint watch){
    (void)env;(void)cls;pthread_mutex_lock(&gate);if(descriptor>=0)inotify_rm_watch(descriptor,watch);pthread_mutex_unlock(&gate);
}
JNIEXPORT jbyteArray JNICALL Java_maclauncher_android_framework_HostFileObserver_nativeNext(JNIEnv *env,jclass cls,jintArray output){
    (void)cls;if(!output||(*env)->GetArrayLength(env,output)!=2){fail(env,"java/lang/IllegalArgumentException","File event output must have two entries");return NULL;}
    jint values[2]={0,0};jbyteArray result=NULL;
    struct inotify_event first={0};size_t remaining=available-cursor;
    if(remaining>=sizeof(first))memcpy(&first,pending+cursor,sizeof(first));
    if(remaining<sizeof(first)||sizeof(first)+first.len>remaining){
        if(remaining)memmove(pending,pending+cursor,remaining);cursor=0;available=remaining;pthread_mutex_lock(&gate);int fd=descriptor<0?-1:fcntl(descriptor,F_DUPFD_CLOEXEC,0);pthread_mutex_unlock(&gate);
        if(fd>=0){struct pollfd watched={fd,POLLIN,0};int ready;do{ready=poll(&watched,1,1000);}while(ready<0&&errno==EINTR);
            if(ready>0){ssize_t n;do{n=read(fd,pending+available,sizeof(pending)-available);}while(n<0&&errno==EINTR);if(n>0)available+=(size_t)n;}close(fd);}
    }
    if(available-cursor>=sizeof(struct inotify_event)){
        struct inotify_event event;memcpy(&event,pending+cursor,sizeof(event));size_t size=sizeof(event)+event.len;
        if(size<=available-cursor){values[0]=event.wd;values[1]=(jint)event.mask;
            if(event.len){const char *name=(char*)pending+cursor+sizeof(event);size_t n=strnlen(name,event.len);result=(*env)->NewByteArray(env,(jsize)n);if(result)(*env)->SetByteArrayRegion(env,result,0,(jsize)n,(const jbyte*)name);}cursor+=size;
        }
    }
    if(!(*env)->ExceptionCheck(env))(*env)->SetIntArrayRegion(env,output,0,2,values);return result;
}
JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostFileObserver_nativeClose(JNIEnv *env,jclass cls){
    (void)env;(void)cls;pthread_mutex_lock(&gate);int fd=descriptor;descriptor=-1;if(fd>=0){shutdown(fd,SHUT_RDWR);close(fd);}pthread_mutex_unlock(&gate);
}
