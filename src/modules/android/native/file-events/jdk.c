#include "operations.h"
#include <jni.h>
#include <sys/inotify.h>
#include <dlfcn.h>
#include <pthread.h>
#include <string.h>
#include "../guest-paths.h"
#include <limits.h>
#include <stdint.h>
extern const char *kl_guest_path(const char *,char *,size_t);
/* Original libc owns virtual process/device files. The JDK shares that owner
 * after installed aliases, without changing its streams or descriptors. */
static int filesystem_virtual_path(const char *path){
    return path&&(!strcmp(path,"/proc")||!strncmp(path,"/proc/",6)||
        !strcmp(path,"/sys")||!strncmp(path,"/sys/",5));
}
static const char *filesystem_native_path(const char *path,char *buffer,size_t capacity){
    const char *mapped=klj_mapped_guest_path(path,buffer,capacity);
    return mapped?mapped:filesystem_virtual_path(path)?kl_guest_path(path,buffer,capacity):path;
}
#include "jdk-filesystem.inc"
#include "jdk-nio-filesystem.inc"

/* Original JVM I/O remains the descriptor owner. Native filename operations use
 * the common Android namespace and publish successful descriptor events. */
static jfieldID descriptor_fd, input_fd, output_fd, random_fd;
static void (*descriptor_close)(JNIEnv*,jobject);
static void (*dispatcher_close)(JNIEnv*,jclass,jobject);
static void (*unix_close)(JNIEnv*,jclass,jint);
static void (*int_close)(JNIEnv*,jclass,jint);
static void (*input_open)(JNIEnv*,jobject,jstring);
static void (*output_open)(JNIEnv*,jobject,jstring,jboolean);
static void (*random_open)(JNIEnv*,jobject,jstring,jint);
static jint (*unix_open)(JNIEnv*,jclass,jlong,jint,jint);
static jint (*unix_openat)(JNIEnv*,jclass,jint,jlong,jint,jint);
static jint (*input_read)(JNIEnv*,jobject),(*random_read)(JNIEnv*,jobject);
static jint (*input_bytes)(JNIEnv*,jobject,jbyteArray,jint,jint),(*random_bytes)(JNIEnv*,jobject,jbyteArray,jint,jint);
static jint (*channel_read)(JNIEnv*,jclass,jobject,jlong,jint);
static jint (*channel_pread)(JNIEnv*,jclass,jobject,jlong,jint,jlong);
static jlong (*channel_readv)(JNIEnv*,jclass,jobject,jlong,jint);
static jint (*unix_read)(JNIEnv*,jclass,jint,jlong,jint);
static jint number(JNIEnv *env,jobject fd){return fd?(*env)->GetIntField(env,fd,descriptor_fd):-1;}
static void opened(JNIEnv *env,jobject stream,jfieldID field){
    if((*env)->ExceptionCheck(env)||!klj_file_event_interest(IN_OPEN))return;
    jobject fd=(*env)->GetObjectField(env,stream,field);
    klj_file_event_fd(number(env,fd),IN_OPEN);if(fd)(*env)->DeleteLocalRef(env,fd);
}
static void publish(JNIEnv *env,const klj_file_event *event){if(!(*env)->ExceptionCheck(env))klj_file_event_publish(event);}
static void fd_close(JNIEnv *e,jobject s){klj_file_event event=klj_file_event_snapshot(number(e,s),IN_CLOSE);descriptor_close(e,s);publish(e,&event);}
static void nio_close(JNIEnv *e,jclass c,jobject fd){klj_file_event event=klj_file_event_snapshot(number(e,fd),IN_CLOSE);dispatcher_close(e,c,fd);publish(e,&event);}
static void fs_close(JNIEnv *e,jclass c,jint fd){klj_file_event event=klj_file_event_snapshot(fd,IN_CLOSE);unix_close(e,c,fd);publish(e,&event);}
static void channel_close(JNIEnv *e,jclass c,jint fd){klj_file_event event=klj_file_event_snapshot(fd,IN_CLOSE);int_close(e,c,fd);publish(e,&event);}
static void fis_open(JNIEnv *e,jobject s,jstring p){jstring path=filesystem_path(e,p);if(!(*e)->ExceptionCheck(e))input_open(e,s,path);if(path)(*e)->DeleteLocalRef(e,path);opened(e,s,input_fd);}
static void fos_open(JNIEnv *e,jobject s,jstring p,jboolean a){jstring path=filesystem_path(e,p);if(!(*e)->ExceptionCheck(e))output_open(e,s,path,a);if(path)(*e)->DeleteLocalRef(e,path);opened(e,s,output_fd);}
static void raf_open(JNIEnv *e,jobject s,jstring p,jint m){jstring path=filesystem_path(e,p);if(!(*e)->ExceptionCheck(e))random_open(e,s,path,m);if(path)(*e)->DeleteLocalRef(e,path);opened(e,s,random_fd);}
static jint fs_open(JNIEnv *e,jclass c,jlong p,jint f,jint m){char buffer[PATH_MAX];const char *mapped=filesystem_native_path((const char*)(uintptr_t)p,buffer,sizeof buffer);jint fd=unix_open(e,c,(jlong)(uintptr_t)mapped,f,m);if(!(*e)->ExceptionCheck(e)&&fd>=0)klj_file_event_fd(fd,IN_OPEN);return fd;}
static jint fs_openat(JNIEnv *e,jclass c,jint d,jlong p,jint f,jint m){char buffer[PATH_MAX];const char *mapped=filesystem_native_path((const char*)(uintptr_t)p,buffer,sizeof buffer);jint fd=unix_openat(e,c,d,(jlong)(uintptr_t)mapped,f,m);if(!(*e)->ExceptionCheck(e)&&fd>=0)klj_file_event_fd(fd,IN_OPEN);return fd;}
static void stream_access(JNIEnv *e,jobject stream,jfieldID field){if((*e)->ExceptionCheck(e)||!klj_file_event_interest(IN_ACCESS))return;jobject fd=(*e)->GetObjectField(e,stream,field);klj_file_event_fd(number(e,fd),IN_ACCESS);if(fd)(*e)->DeleteLocalRef(e,fd);}
static jint fis_read(JNIEnv *e,jobject s){jint n=input_read(e,s);if(n>=0)stream_access(e,s,input_fd);return n;}
static jint raf_read(JNIEnv *e,jobject s){jint n=random_read(e,s);if(n>=0)stream_access(e,s,random_fd);return n;}
static jint fis_bytes(JNIEnv *e,jobject s,jbyteArray b,jint o,jint l){jint n=input_bytes(e,s,b,o,l);if(n>0)stream_access(e,s,input_fd);return n;}
static jint raf_bytes(JNIEnv *e,jobject s,jbyteArray b,jint o,jint l){jint n=random_bytes(e,s,b,o,l);if(n>0)stream_access(e,s,random_fd);return n;}
static jint nio_read(JNIEnv *e,jclass c,jobject fd,jlong p,jint l){jint n=channel_read(e,c,fd,p,l);if(n>0&&!(*e)->ExceptionCheck(e))klj_file_event_fd(number(e,fd),IN_ACCESS);return n;}
static jint nio_pread(JNIEnv *e,jclass c,jobject fd,jlong p,jint l,jlong o){jint n=channel_pread(e,c,fd,p,l,o);if(n>0&&!(*e)->ExceptionCheck(e))klj_file_event_fd(number(e,fd),IN_ACCESS);return n;}
static jlong nio_readv(JNIEnv *e,jclass c,jobject fd,jlong p,jint l){jlong n=channel_readv(e,c,fd,p,l);if(n>0&&!(*e)->ExceptionCheck(e))klj_file_event_fd(number(e,fd),IN_ACCESS);return n;}
static jint fs_read(JNIEnv *e,jclass c,jint fd,jlong p,jint l){jint n=unix_read(e,c,fd,p,l);if(n>0&&!(*e)->ExceptionCheck(e))klj_file_event_fd(fd,IN_ACCESS);return n;}

static pthread_mutex_t installation=PTHREAD_MUTEX_INITIALIZER;
static int installed;
JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostFileObserver_nativeInstall(JNIEnv *env,jclass ignored){
    (void)ignored;pthread_mutex_lock(&installation);
    if(installed){pthread_mutex_unlock(&installation);return;}
    if(!filesystem_initialize(env)){pthread_mutex_unlock(&installation);return;}
    if(!filesystem_nio_initialize(env)){pthread_mutex_unlock(&installation);return;}
    struct binding {const char *type,*name,*signature,*symbol;void **original,*wrapper;jclass owner;} bindings[]={
        {"java/io/FileDescriptor","close0","()V","Java_java_io_FileDescriptor_close0",(void**)&descriptor_close,fd_close,NULL},
        {"sun/nio/ch/UnixDispatcher","close0","(Ljava/io/FileDescriptor;)V","Java_sun_nio_ch_UnixDispatcher_close0",(void**)&dispatcher_close,nio_close,NULL},
        {"sun/nio/fs/UnixNativeDispatcher","close0","(I)V","Java_sun_nio_fs_UnixNativeDispatcher_close0",(void**)&unix_close,fs_close,NULL},
        {"sun/nio/ch/UnixFileDispatcherImpl","closeIntFD","(I)V","Java_sun_nio_ch_UnixFileDispatcherImpl_closeIntFD",(void**)&int_close,channel_close,NULL},
        {"java/io/FileInputStream","open0","(Ljava/lang/String;)V","Java_java_io_FileInputStream_open0",(void**)&input_open,fis_open,NULL},
        {"java/io/FileOutputStream","open0","(Ljava/lang/String;Z)V","Java_java_io_FileOutputStream_open0",(void**)&output_open,fos_open,NULL},
        {"java/io/RandomAccessFile","open0","(Ljava/lang/String;I)V","Java_java_io_RandomAccessFile_open0",(void**)&random_open,raf_open,NULL},
        {"sun/nio/fs/UnixNativeDispatcher","open0","(JII)I","Java_sun_nio_fs_UnixNativeDispatcher_open0",(void**)&unix_open,fs_open,NULL},
        {"sun/nio/fs/UnixNativeDispatcher","openat0","(IJII)I","Java_sun_nio_fs_UnixNativeDispatcher_openat0",(void**)&unix_openat,fs_openat,NULL},
        {"java/io/FileInputStream","read0","()I","Java_java_io_FileInputStream_read0",(void**)&input_read,fis_read,NULL},
        {"java/io/RandomAccessFile","read0","()I","Java_java_io_RandomAccessFile_read0",(void**)&random_read,raf_read,NULL},
        {"java/io/FileInputStream","readBytes","([BII)I","Java_java_io_FileInputStream_readBytes",(void**)&input_bytes,fis_bytes,NULL},
        {"java/io/RandomAccessFile","readBytes0","([BII)I","Java_java_io_RandomAccessFile_readBytes0",(void**)&random_bytes,raf_bytes,NULL},
        {"sun/nio/ch/UnixFileDispatcherImpl","read0","(Ljava/io/FileDescriptor;JI)I","Java_sun_nio_ch_UnixFileDispatcherImpl_read0",(void**)&channel_read,nio_read,NULL},
        {"sun/nio/ch/UnixFileDispatcherImpl","pread0","(Ljava/io/FileDescriptor;JIJ)I","Java_sun_nio_ch_UnixFileDispatcherImpl_pread0",(void**)&channel_pread,nio_pread,NULL},
        {"sun/nio/ch/UnixFileDispatcherImpl","readv0","(Ljava/io/FileDescriptor;JI)J","Java_sun_nio_ch_UnixFileDispatcherImpl_readv0",(void**)&channel_readv,nio_readv,NULL},
        {"sun/nio/fs/UnixNativeDispatcher","read0","(IJI)I","Java_sun_nio_fs_UnixNativeDispatcher_read0",(void**)&unix_read,fs_read,NULL},
        FILESYSTEM_BINDINGS,
        FILESYSTEM_NIO_BINDINGS,
    };
    size_t count=sizeof(bindings)/sizeof(bindings[0]),ready=0;
    /* Resolve all originals before any registration. RTLD_DEFAULT sees the
     * JDK's loaded native libraries; no second libjava/libnio is loaded. */
    for(;ready<count;ready++){
        struct binding *b=&bindings[ready];b->owner=(*env)->FindClass(env,b->type);if(!b->owner)break;
        *b->original=dlsym(RTLD_DEFAULT,b->symbol);
        if(!*b->original){jclass error=(*env)->FindClass(env,"java/lang/UnsatisfiedLinkError");if(error){(*env)->ThrowNew(env,error,b->symbol);(*env)->DeleteLocalRef(env,error);}break;}
    }
    if(ready==count){
        descriptor_fd=(*env)->GetFieldID(env,bindings[0].owner,"fd","I");
        if(descriptor_fd)input_fd=(*env)->GetFieldID(env,bindings[4].owner,"fd","Ljava/io/FileDescriptor;");
        if(input_fd)output_fd=(*env)->GetFieldID(env,bindings[5].owner,"fd","Ljava/io/FileDescriptor;");
        if(output_fd)random_fd=(*env)->GetFieldID(env,bindings[6].owner,"fd","Ljava/io/FileDescriptor;");
        if(random_fd){
            size_t bound=0;
            for(;bound<count;bound++){struct binding *b=&bindings[bound];JNINativeMethod method={(char*)b->name,(char*)b->signature,b->wrapper};if((*env)->RegisterNatives(env,b->owner,&method,1)!=JNI_OK)break;}
            installed=bound==count;
            if(!installed){jthrowable error=(*env)->ExceptionOccurred(env);(*env)->ExceptionClear(env);for(size_t i=0;i<bound;i++){struct binding *b=&bindings[i];JNINativeMethod method={(char*)b->name,(char*)b->signature,*b->original};(*env)->RegisterNatives(env,b->owner,&method,1);(*env)->ExceptionClear(env);}if(error){(*env)->Throw(env,error);(*env)->DeleteLocalRef(env,error);}}
        }
    }
    for(size_t i=0;i<count;i++)if(bindings[i].owner)(*env)->DeleteLocalRef(env,bindings[i].owner);
    pthread_mutex_unlock(&installation);
}
