/* Original libcore DNS entry points over the host resolver and session policy. */
#include <jni.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include "network-policy.h"

static void fail(JNIEnv *env,const char *name,const char *message){
    jclass type=(*env)->FindClass(env,name);if(type){(*env)->ThrowNew(env,type,message);(*env)->DeleteLocalRef(env,type);}
}
static int android_gai(int error){
    switch(error){
        case EAI_AGAIN:return 2;case EAI_BADFLAGS:return 3;case EAI_FAIL:return 4;
        case EAI_FAMILY:case EAI_ADDRFAMILY:return 5;case EAI_MEMORY:return 6;
        case EAI_NODATA:return 7;case EAI_NONAME:return 8;case EAI_SERVICE:return 9;
        case EAI_SOCKTYPE:return 10;case EAI_SYSTEM:return 11;case EAI_OVERFLOW:return 14;
        case EAI_BADHINTS:return 3;case EAI_PROTOCOL:return 9;default:return 4;
    }
}
static int host_gai(int error){
    switch(error){case 2:return EAI_AGAIN;case 3:return EAI_BADFLAGS;case 4:return EAI_FAIL;
        case 5:return EAI_FAMILY;case 6:return EAI_MEMORY;case 7:return EAI_NODATA;
        case 8:return EAI_NONAME;case 9:return EAI_SERVICE;case 10:return EAI_SOCKTYPE;
        case 11:return EAI_SYSTEM;case 14:return EAI_OVERFLOW;default:return error==0?0:-1;}
}
static int android_errno(int error){
    switch(error){case EPERM:return 1;case ENOENT:return 2;case ESRCH:return 3;case EINTR:return 4;
        case EIO:return 5;case EBADF:return 9;case EAGAIN:return 11;case ENOMEM:return 12;
        case EACCES:return 13;case EFAULT:return 14;case EINVAL:return 22;case ENFILE:return 23;
        case EMFILE:return 24;case EPIPE:return 32;case ENAMETOOLONG:return 36;case ENOSYS:return 38;
        case ELOOP:return 40;case EOVERFLOW:return 75;case ENOTSOCK:return 88;case EPROTONOSUPPORT:return 93;
        case EAFNOSUPPORT:return 97;case ENETDOWN:return 100;case ENETUNREACH:return 101;
        case ECONNRESET:return 104;case ENOBUFS:return 105;case ETIMEDOUT:return 110;
        case ECONNREFUSED:return 111;case EHOSTUNREACH:return 113;case ECANCELED:return 125;default:return 5;}
}
static void gai_error(JNIEnv *env,int error,int system_error){
    if((*env)->ExceptionCheck(env))return;
    jclass type=(*env)->FindClass(env,"android/system/GaiException");if(!type)return;
    jmethodID init=(*env)->GetMethodID(env,type,"<init>","(Ljava/lang/String;ILjava/lang/Throwable;)V");
    jstring operation=init?(*env)->NewStringUTF(env,"android_getaddrinfo"):NULL;
    jobject cause=NULL;
    if(operation&&error==EAI_SYSTEM){
        jclass errno_type=(*env)->FindClass(env,"android/system/ErrnoException");
        jmethodID errno_init=errno_type?(*env)->GetMethodID(env,errno_type,"<init>","(Ljava/lang/String;I)V"):NULL;
        if(errno_init)cause=(*env)->NewObject(env,errno_type,errno_init,operation,android_errno(system_error));
        if(errno_type)(*env)->DeleteLocalRef(env,errno_type);
    }
    if(operation&&!(*env)->ExceptionCheck(env)){
        jthrowable failure=(*env)->NewObject(env,type,init,operation,android_gai(error),cause);
        if(failure){(*env)->Throw(env,failure);(*env)->DeleteLocalRef(env,failure);}
    }
    if(cause)(*env)->DeleteLocalRef(env,cause);if(operation)(*env)->DeleteLocalRef(env,operation);(*env)->DeleteLocalRef(env,type);
}
static int current_network(JNIEnv *env,jint resolver_id){
    // Network.getNetIdForResolv adds this private-DNS bypass bit. The Mac route
    // has no Android private-DNS layer, but its actual network identity remains.
    jint net_id=(jint)((uint32_t)resolver_id&0x7fffffffU);if(net_id==0)return 1;
    jclass services=(*env)->FindClass(env,"android/os/ServiceManager");if(!services)return 0;
    jmethodID get=(*env)->GetStaticMethodID(env,services,"getService","(Ljava/lang/String;)Landroid/os/IBinder;");
    jstring name=get?(*env)->NewStringUTF(env,"connectivity"):NULL;
    jobject binder=name?(*env)->CallStaticObjectMethod(env,services,get,name):NULL;
    if(name)(*env)->DeleteLocalRef(env,name);(*env)->DeleteLocalRef(env,services);
    if((*env)->ExceptionCheck(env)||!binder)return 0;
    jclass stub=(*env)->FindClass(env,"android/net/IConnectivityManager$Stub");
    jmethodID as_interface=stub?(*env)->GetStaticMethodID(env,stub,"asInterface","(Landroid/os/IBinder;)Landroid/net/IConnectivityManager;"):NULL;
    jobject manager=as_interface?(*env)->CallStaticObjectMethod(env,stub,as_interface,binder):NULL;
    if(stub)(*env)->DeleteLocalRef(env,stub);(*env)->DeleteLocalRef(env,binder);
    if((*env)->ExceptionCheck(env)||!manager)return 0;
    jclass manager_type=(*env)->GetObjectClass(env,manager);
    jmethodID active=manager_type?(*env)->GetMethodID(env,manager_type,"getActiveNetwork","()Landroid/net/Network;"):NULL;
    jobject network=active?(*env)->CallObjectMethod(env,manager,active):NULL;
    if(manager_type)(*env)->DeleteLocalRef(env,manager_type);(*env)->DeleteLocalRef(env,manager);
    if((*env)->ExceptionCheck(env)||!network)return 0;
    jclass network_type=(*env)->GetObjectClass(env,network);
    jfieldID identity=network_type?(*env)->GetFieldID(env,network_type,"netId","I"):NULL;
    int matches=identity&&(*env)->GetIntField(env,network,identity)==net_id;
    if(network_type)(*env)->DeleteLocalRef(env,network_type);(*env)->DeleteLocalRef(env,network);return matches;
}
static int hints(JNIEnv *env,jobject input,struct addrinfo *output){
    if(!input){fail(env,"java/lang/NullPointerException","Address hints are null");return 0;}
    jclass type=(*env)->GetObjectClass(env,input);if(!type)return 0;
    const char *names[]={"ai_flags","ai_family","ai_socktype","ai_protocol"};jint values[4];
    for(int i=0;i<4;i++){jfieldID field=(*env)->GetFieldID(env,type,names[i],"I");if(!field){(*env)->DeleteLocalRef(env,type);return 0;}values[i]=(*env)->GetIntField(env,input,field);}
    (*env)->DeleteLocalRef(env,type);
    const int android_flags[]={1,2,4,8,0x100,0x400,0x800};
    const int host_flags[]={AI_PASSIVE,AI_CANONNAME,AI_NUMERICHOST,AI_NUMERICSERV,AI_ALL,AI_ADDRCONFIG,AI_V4MAPPED};
    int remaining=values[0];for(int i=0;i<7;i++)if(remaining&android_flags[i]){output->ai_flags|=host_flags[i];remaining&=~android_flags[i];}
    if(remaining){gai_error(env,EAI_BADFLAGS,0);return 0;}
    switch(values[1]){case 0:output->ai_family=AF_UNSPEC;break;case 2:output->ai_family=AF_INET;break;case 10:output->ai_family=AF_INET6;break;default:gai_error(env,EAI_FAMILY,0);return 0;}
    output->ai_socktype=values[2];output->ai_protocol=values[3];return 1;
}
JNIEXPORT jobjectArray JNICALL Java_libcore_io_Linux_android_1getaddrinfo(JNIEnv *env,jobject owner,jstring name,jobject input,jint net_id){
    (void)owner;if(!name){fail(env,"java/lang/NullPointerException","Host name is null");return NULL;}
    struct addrinfo query={0};if(!hints(env,input,&query))return NULL;
    const char *host=(*env)->GetStringUTFChars(env,name,NULL);if(!host)return NULL;
    int allowed=klj_network_host_allowed(host);(*env)->ReleaseStringUTFChars(env,name,host);
    if(!allowed){gai_error(env,EAI_NONAME,0);return NULL;}
    if(!current_network(env,net_id)){gai_error(env,EAI_NONAME,0);return NULL;}
    host=(*env)->GetStringUTFChars(env,name,NULL);if(!host)return NULL;
    struct addrinfo *addresses=NULL;int error=getaddrinfo(host,NULL,&query,&addresses),system_error=errno;
    (*env)->ReleaseStringUTFChars(env,name,host);
    if(error){gai_error(env,error,system_error);return NULL;}
    size_t count=0;for(struct addrinfo *at=addresses;at;at=at->ai_next)if(at->ai_family==AF_INET||at->ai_family==AF_INET6)count++;
    jobjectArray result=NULL;
    if(count>INT_MAX){gai_error(env,EAI_OVERFLOW,0);goto done;}
    jclass inet=(*env)->FindClass(env,"java/net/InetAddress");if(!inet)goto done;
    jclass inet6=(*env)->FindClass(env,"java/net/Inet6Address");if(!inet6){(*env)->DeleteLocalRef(env,inet);goto done;}
    jmethodID v4=(*env)->GetStaticMethodID(env,inet,"getByAddress","(Ljava/lang/String;[B)Ljava/net/InetAddress;");
    jmethodID v6=v4?(*env)->GetStaticMethodID(env,inet6,"getByAddress","(Ljava/lang/String;[BI)Ljava/net/Inet6Address;"):NULL;
    if(v6)result=(*env)->NewObjectArray(env,(jsize)count,inet,NULL);
    jsize index=0;
    for(struct addrinfo *at=addresses;result&&at&&!(*env)->ExceptionCheck(env);at=at->ai_next){
        const void *bytes;jsize length;jint scope=0;
        if(at->ai_family==AF_INET){bytes=&((struct sockaddr_in *)at->ai_addr)->sin_addr;length=4;}
        else if(at->ai_family==AF_INET6){struct sockaddr_in6 *address=(struct sockaddr_in6 *)at->ai_addr;bytes=&address->sin6_addr;length=16;scope=(jint)address->sin6_scope_id;}
        else continue;
        jbyteArray raw=(*env)->NewByteArray(env,length);if(!raw)break;
        (*env)->SetByteArrayRegion(env,raw,0,length,(const jbyte *)bytes);
        jobject address=NULL;if(!(*env)->ExceptionCheck(env))address=length==4?(*env)->CallStaticObjectMethod(env,inet,v4,name,raw):(*env)->CallStaticObjectMethod(env,inet6,v6,name,raw,scope);
        (*env)->DeleteLocalRef(env,raw);
        if(!(*env)->ExceptionCheck(env)&&address)(*env)->SetObjectArrayElement(env,result,index++,address);
        if(address)(*env)->DeleteLocalRef(env,address);
    }
    (*env)->DeleteLocalRef(env,inet6);(*env)->DeleteLocalRef(env,inet);
done:
    freeaddrinfo(addresses);return result;
}
JNIEXPORT jstring JNICALL Java_libcore_io_Linux_gai_1strerror(JNIEnv *env,jobject owner,jint error){
    (void)owner;return (*env)->NewStringUTF(env,gai_strerror(host_gai(error)));
}
