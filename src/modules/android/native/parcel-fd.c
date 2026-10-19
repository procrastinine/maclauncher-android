#include <jni.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static void fail(JNIEnv *env,const char *kind,const char *operation,int code){
    char message[256];snprintf(message,sizeof(message),"%s: %s",operation,strerror(code));
    jclass type=(*env)->FindClass(env,kind);if(type){(*env)->ThrowNew(env,type,message);(*env)->DeleteLocalRef(env,type);}
}
static jfieldID field(JNIEnv *env,jobject descriptor){
    jclass type=(*env)->GetObjectClass(env,descriptor);if(!type)return NULL;
    jfieldID result=(*env)->GetFieldID(env,type,"fd","I");(*env)->DeleteLocalRef(env,type);return result;
}
static int number(JNIEnv *env,jobject descriptor){jfieldID id=field(env,descriptor);return id?(*env)->GetIntField(env,descriptor,id):-1;}
// Obtain the actual Java 21 socket descriptor, then Java dup() gives the PFD
// independent ownership. No connection, bind or replacement socket is created.
JNIEXPORT jobject JNICALL Java_maclauncher_android_framework_HostFileDescriptor_socketDescriptor(JNIEnv *env,jclass cls,jobject socket,jboolean datagram){
    const char *owner=datagram?"java/net/DatagramSocket":"java/net/Socket";
    jclass type=(*env)->FindClass(env,owner);if(!type)return NULL;
    jobject impl=NULL;
    if(datagram){
        jfieldID delegate=(*env)->GetFieldID(env,type,"delegate","Ljava/net/DatagramSocket;");
        if(delegate){impl=(*env)->GetObjectField(env,socket,delegate);if(!impl)impl=(*env)->NewLocalRef(env,socket);}
    }else{
        jmethodID getter=(*env)->GetMethodID(env,type,"getImpl","()Ljava/net/SocketImpl;");
        if(getter)impl=(*env)->CallObjectMethod(env,socket,getter);
    }
    (*env)->DeleteLocalRef(env,type);if((*env)->ExceptionCheck(env)||!impl)return NULL;
    jobject descriptor=NULL;
    if(datagram){
        jclass adapter=(*env)->FindClass(env,"sun/nio/ch/DatagramSocketAdaptor");
        if(adapter&&!(*env)->IsInstanceOf(env,impl,adapter)){
            (*env)->DeleteLocalRef(env,adapter);(*env)->DeleteLocalRef(env,impl);
            fail(env,"java/io/IOException","Custom datagram socket descriptor",ENOTSUP);return NULL;
        }
        jfieldID dc=adapter?(*env)->GetFieldID(env,adapter,"dc","Lsun/nio/ch/DatagramChannelImpl;"):NULL;
        jobject channel=dc?(*env)->GetObjectField(env,impl,dc):NULL;
        if(adapter)(*env)->DeleteLocalRef(env,adapter);
        if(channel){
            jclass channel_type=(*env)->GetObjectClass(env,channel);
            jmethodID get=channel_type?(*env)->GetMethodID(env,channel_type,"getFD","()Ljava/io/FileDescriptor;"):NULL;
            if(get)descriptor=(*env)->CallObjectMethod(env,channel,get);
            if((*env)->ExceptionCheck(env))descriptor=NULL;
            if(channel_type)(*env)->DeleteLocalRef(env,channel_type);(*env)->DeleteLocalRef(env,channel);
        }
    }else{
        jclass impl_type=(*env)->GetObjectClass(env,impl);
        jmethodID get=impl_type?(*env)->GetMethodID(env,impl_type,"getFileDescriptor","()Ljava/io/FileDescriptor;"):NULL;
        if(get)descriptor=(*env)->CallObjectMethod(env,impl,get);
        if((*env)->ExceptionCheck(env))descriptor=NULL;
        if(impl_type)(*env)->DeleteLocalRef(env,impl_type);
    }
    (*env)->DeleteLocalRef(env,impl);return descriptor;
}

JNIEXPORT jint JNICALL Java_maclauncher_android_framework_HostFileDescriptor_detach(JNIEnv *env,jclass cls,jobject descriptor){
    jfieldID id=field(env,descriptor);if(!id)return -1;
    int fd=(*env)->GetIntField(env,descriptor,id);
    jclass type=(*env)->GetObjectClass(env,descriptor);if(!type)return -1;
    jmethodID unregister=(*env)->GetMethodID(env,type,"unregisterCleanup","()V");
    if(unregister)(*env)->CallVoidMethod(env,descriptor,unregister);
    (*env)->DeleteLocalRef(env,type);if((*env)->ExceptionCheck(env))return -1;
    (*env)->SetIntField(env,descriptor,id,-1);return fd;
}
JNIEXPORT jint JNICALL Java_maclauncher_android_framework_HostFileDescriptor_getInt(JNIEnv *env,jclass cls,jobject descriptor){
    (void)cls;if(!descriptor){fail(env,"java/lang/NullPointerException","File descriptor is null",0);return -1;}return number(env,descriptor);
}
JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostFileDescriptor_registerCleanup(JNIEnv *env,jclass cls,jobject descriptor){
    (void)cls;jclass type=(*env)->FindClass(env,"java/io/FileCleanable");if(!type)return;
    jmethodID registration=(*env)->GetStaticMethodID(env,type,"register","(Ljava/io/FileDescriptor;)V");if(registration)(*env)->CallStaticVoidMethod(env,type,registration,descriptor);(*env)->DeleteLocalRef(env,type);
}
