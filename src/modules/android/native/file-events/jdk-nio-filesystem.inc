/* The original JDK stat result shares libcore's existing Android UID/GID owner.
 * JDK attributes, errors and descriptor semantics stay with the JDK. */
#include "../process-identity.h"
static jfieldID filesystem_uid,filesystem_gid;
static int filesystem_nio_initialize(JNIEnv *env){
    jclass attributes=(*env)->FindClass(env,"sun/nio/fs/UnixFileAttributes");if(!attributes)return 0;
    filesystem_uid=(*env)->GetFieldID(env,attributes,"st_uid","I");
    if(filesystem_uid)filesystem_gid=(*env)->GetFieldID(env,attributes,"st_gid","I");
    (*env)->DeleteLocalRef(env,attributes);return filesystem_uid&&filesystem_gid&&!(*env)->ExceptionCheck(env);
}
static jlong filesystem_address(jlong path,char *buffer,size_t capacity){
    const char *mapped=klj_mapped_guest_path((const char*)(uintptr_t)path,buffer,capacity);
    return mapped?(jlong)(uintptr_t)mapped:path;
}
static void filesystem_nio_attributes(JNIEnv *env,jobject attributes){
    if((*env)->ExceptionCheck(env))return;
    jint uid=(*env)->GetIntField(env,attributes,filesystem_uid),gid=(*env)->GetIntField(env,attributes,filesystem_gid);
    (*env)->SetIntField(env,attributes,filesystem_uid,(jint)klj_process_uid_from_host((uid_t)uid));
    (*env)->SetIntField(env,attributes,filesystem_gid,(jint)klj_process_gid_from_host((gid_t)gid));
}
static jint (*filesystem_original_stat)(JNIEnv*,jclass,jlong,jobject);
static jint filesystem_stat(JNIEnv *env,jclass type,jlong path,jobject attributes){
    char buffer[PATH_MAX];jint result=filesystem_original_stat(env,type,filesystem_address(path,buffer,sizeof buffer),attributes);
    if(result==0)filesystem_nio_attributes(env,attributes);return result;
}
static void (*filesystem_original_lstat)(JNIEnv*,jclass,jlong,jobject);
static void filesystem_lstat(JNIEnv *env,jclass type,jlong path,jobject attributes){
    char buffer[PATH_MAX];filesystem_original_lstat(env,type,filesystem_address(path,buffer,sizeof buffer),attributes);filesystem_nio_attributes(env,attributes);
}
static void (*filesystem_original_fstat)(JNIEnv*,jclass,jint,jobject);
static void filesystem_fstat(JNIEnv *env,jclass type,jint fd,jobject attributes){filesystem_original_fstat(env,type,fd,attributes);filesystem_nio_attributes(env,attributes);}
static void (*filesystem_original_fstatat)(JNIEnv*,jclass,jint,jlong,jint,jobject);
static void filesystem_fstatat(JNIEnv *env,jclass type,jint fd,jlong path,jint flags,jobject attributes){
    char buffer[PATH_MAX];filesystem_original_fstatat(env,type,fd,filesystem_address(path,buffer,sizeof buffer),flags,attributes);filesystem_nio_attributes(env,attributes);
}
static jint (*filesystem_original_nio_access)(JNIEnv*,jclass,jlong,jint);
static jint filesystem_nio_access(JNIEnv *env,jclass type,jlong path,jint mode){
    char buffer[PATH_MAX];return filesystem_original_nio_access(env,type,filesystem_address(path,buffer,sizeof buffer),mode);
}
#define FILESYSTEM_CHOWN(NAME) \
    static void (*filesystem_original_##NAME)(JNIEnv*,jclass,jlong,jint,jint); \
    static void filesystem_##NAME(JNIEnv *env,jclass type,jlong path,jint uid,jint gid){ \
        char buffer[PATH_MAX];filesystem_original_##NAME(env,type,filesystem_address(path,buffer,sizeof buffer), \
            (jint)klj_process_uid_to_host((uid_t)uid),(jint)klj_process_gid_to_host((gid_t)gid)); \
    }
FILESYSTEM_CHOWN(chown)
FILESYSTEM_CHOWN(lchown)
#undef FILESYSTEM_CHOWN
static void (*filesystem_original_fchown)(JNIEnv*,jclass,jint,jint,jint);
static void filesystem_fchown(JNIEnv *env,jclass type,jint fd,jint uid,jint gid){
    filesystem_original_fchown(env,type,fd,(jint)klj_process_uid_to_host((uid_t)uid),(jint)klj_process_gid_to_host((gid_t)gid));
}
#define FILESYSTEM_NIO_BINDING(NAME,SIGNATURE) \
    {"sun/nio/fs/UnixNativeDispatcher",#NAME "0",SIGNATURE,"Java_sun_nio_fs_UnixNativeDispatcher_" #NAME "0",(void**)&filesystem_original_##NAME,filesystem_##NAME,NULL}
#define FILESYSTEM_NIO_BINDINGS \
    FILESYSTEM_NIO_BINDING(stat,"(JLsun/nio/fs/UnixFileAttributes;)I"), \
    FILESYSTEM_NIO_BINDING(lstat,"(JLsun/nio/fs/UnixFileAttributes;)V"), \
    FILESYSTEM_NIO_BINDING(fstat,"(ILsun/nio/fs/UnixFileAttributes;)V"), \
    FILESYSTEM_NIO_BINDING(fstatat,"(IJILsun/nio/fs/UnixFileAttributes;)V"), \
    {"sun/nio/fs/UnixNativeDispatcher","access0","(JI)I","Java_sun_nio_fs_UnixNativeDispatcher_access0",(void**)&filesystem_original_nio_access,filesystem_nio_access,NULL}, \
    FILESYSTEM_NIO_BINDING(chown,"(JII)V"), \
    FILESYSTEM_NIO_BINDING(lchown,"(JII)V"), \
    FILESYSTEM_NIO_BINDING(fchown,"(III)V")
