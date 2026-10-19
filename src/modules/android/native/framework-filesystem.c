#include "poll-bridge.h"
#include "seek-bridge.h"
#include "file-events/operations.h"
#include <jni.h>
#include <sys/statvfs.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <crt_externs.h>
#include "file-xattrs.h"
#include <limits.h>
#include "process-identity.h"
#include "pollable-fds/imports.h"

/* Original libcore Os/StatFs use real macOS filesystem data. Path translation
 * belongs to HostFilesystem; access remains subject to the process sandbox. */
extern int kl_errno_to_linux(int);
extern int kl_errno_from_linux(int);
static int android_errno(int value){return kl_errno_to_linux(value);}
static void fail(JNIEnv *env,const char *type,const char *message){jclass c=(*env)->FindClass(env,type);if(c){(*env)->ThrowNew(env,c,message);(*env)->DeleteLocalRef(env,c);}}
static void syscall_error(JNIEnv *env,const char *name,int value){
    jclass c=(*env)->FindClass(env,"android/system/ErrnoException");if(!c)return;
    jmethodID ctor=(*env)->GetMethodID(env,c,"<init>","(Ljava/lang/String;I)V");
    if(ctor){jstring operation=(*env)->NewStringUTF(env,name);if(operation){jthrowable error=(*env)->NewObject(env,c,ctor,operation,android_errno(value));if(error){(*env)->Throw(env,error);(*env)->DeleteLocalRef(env,error);}(*env)->DeleteLocalRef(env,operation);}}
    (*env)->DeleteLocalRef(env,c);
}
static char *bytes(JNIEnv *env,jbyteArray input){
    if(!input)return NULL;jsize length=(*env)->GetArrayLength(env,input);
    if(length>1048576){fail(env,"java/lang/IllegalArgumentException","Native string is too large");return NULL;}
    char *result=malloc((size_t)length+1);if(!result){fail(env,"java/lang/OutOfMemoryError","Native string allocation failed");return NULL;}
    (*env)->GetByteArrayRegion(env,input,0,length,(jbyte*)result);
    if((*env)->ExceptionCheck(env)){free(result);return NULL;}
    if(memchr(result,0,(size_t)length)){free(result);fail(env,"java/lang/IllegalArgumentException","Native string contains NUL");return NULL;}
    result[length]=0;return result;
}
static char *utf8(JNIEnv *env,jstring value){
    if(!value){fail(env,"java/lang/NullPointerException","String is null");return NULL;}
    jclass strings=(*env)->FindClass(env,"java/lang/String");if(!strings)return NULL;
    jmethodID encode=(*env)->GetMethodID(env,strings,"getBytes","(Ljava/lang/String;)[B");
    jstring charset=encode?(*env)->NewStringUTF(env,"UTF-8"):NULL;
    jbyteArray encoded=charset?(*env)->CallObjectMethod(env,value,encode,charset):NULL;
    char *result=(*env)->ExceptionCheck(env)?NULL:bytes(env,encoded);
    if(encoded)(*env)->DeleteLocalRef(env,encoded);if(charset)(*env)->DeleteLocalRef(env,charset);(*env)->DeleteLocalRef(env,strings);return result;
}
static jstring text(JNIEnv *env,const char *value){
    if(!value)return NULL;size_t length=strlen(value);if(length>1048576){fail(env,"java/lang/IllegalArgumentException","Native string is too large");return NULL;}
    jbyteArray data=(*env)->NewByteArray(env,(jsize)length);if(!data)return NULL;
    (*env)->SetByteArrayRegion(env,data,0,(jsize)length,(const jbyte*)value);
    jclass strings=(*env)->FindClass(env,"java/lang/String");jstring result=NULL;
    if(strings){jmethodID ctor=(*env)->GetMethodID(env,strings,"<init>","([BLjava/lang/String;)V");jstring charset=ctor?(*env)->NewStringUTF(env,"UTF-8"):NULL;if(charset){result=(*env)->NewObject(env,strings,ctor,data,charset);(*env)->DeleteLocalRef(env,charset);}(*env)->DeleteLocalRef(env,strings);}
    (*env)->DeleteLocalRef(env,data);return result;
}
static char *path(JNIEnv *env,jstring value){
    jclass c=(*env)->FindClass(env,"maclauncher/android/framework/HostFilesystem");if(!c)return NULL;
    jmethodID map=(*env)->GetStaticMethodID(env,c,"pathBytes","(Ljava/lang/String;)[B");
    jbyteArray mapped=map?(*env)->CallStaticObjectMethod(env,c,map,value):NULL;
    char *result=(*env)->ExceptionCheck(env)?NULL:bytes(env,mapped);
    if(mapped)(*env)->DeleteLocalRef(env,mapped);(*env)->DeleteLocalRef(env,c);return result;
}
/* Linux and Darwin have different xattr flag values and syscall signatures.
 * All attribute bytes remain on the real private file, with native permissions. */
static char *read_xattrs(JNIEnv *env,const char *file,const char *name,ssize_t *length){
    const char *operation=name?"getxattr":"listxattr";
    for(;;){
        ssize_t size=name?klj_getxattr(file,name,NULL,0):klj_listxattr(file,NULL,0);
        if(size<0){syscall_error(env,operation,errno);return NULL;}
        if(size>INT_MAX){syscall_error(env,operation,EOVERFLOW);return NULL;}
        char *result=malloc(size?(size_t)size:1);
        if(!result){fail(env,"java/lang/OutOfMemoryError","Extended attribute allocation failed");return NULL;}
        ssize_t read=name?klj_getxattr(file,name,result,(size_t)size):klj_listxattr(file,result,(size_t)size);
        if(read>=0&&read<=size){*length=read;return result;}
        int error=errno;free(result);
        if(read>size||error==ERANGE)continue; /* The file changed between calls. */
        syscall_error(env,operation,error);return NULL;
    }
}
JNIEXPORT jbyteArray JNICALL Java_libcore_io_Linux_getxattr(JNIEnv *env,jobject owner,jstring value,jstring key){
    (void)owner;char *file=path(env,value);if(!file)return NULL;
    char *name=utf8(env,key);if(!name){free(file);return NULL;}
    ssize_t size=0;char *data=read_xattrs(env,file,name,&size);free(file);free(name);if(!data)return NULL;
    jbyteArray result=(*env)->NewByteArray(env,(jsize)size);
    if(result)(*env)->SetByteArrayRegion(env,result,0,(jsize)size,(jbyte *)data);
    free(data);return result;
}
JNIEXPORT jobjectArray JNICALL Java_libcore_io_Linux_listxattr(JNIEnv *env,jobject owner,jstring value){
    (void)owner;char *file=path(env,value);if(!file)return NULL;
    ssize_t size=0;char *data=read_xattrs(env,file,NULL,&size);free(file);if(!data)return NULL;
    size_t count=0;
    for(char *at=data;at<data+size;count++){char *end=memchr(at,0,(size_t)(data+size-at));if(!end){free(data);syscall_error(env,"listxattr",EIO);return NULL;}at=end+1;}
    jclass strings=(*env)->FindClass(env,"java/lang/String");jobjectArray result=strings?(*env)->NewObjectArray(env,(jsize)count,strings,NULL):NULL;
    if(strings)(*env)->DeleteLocalRef(env,strings);
    if(result){char *at=data;for(jsize i=0;i<(jsize)count;i++){jstring item=text(env,at);if(!item)break;(*env)->SetObjectArrayElement(env,result,i,item);(*env)->DeleteLocalRef(env,item);if((*env)->ExceptionCheck(env))break;at+=strlen(at)+1;}}
    free(data);return result;
}
JNIEXPORT void JNICALL Java_libcore_io_Linux_setxattr(JNIEnv *env,jobject owner,jstring value,jstring key,jbyteArray bytes,jint flags){
    (void)owner;if(!bytes){fail(env,"java/lang/NullPointerException","Extended attribute value is null");return;}
    char *file=path(env,value);if(!file)return;char *name=utf8(env,key);if(!name){free(file);return;}
    jsize length=(*env)->GetArrayLength(env,bytes);jbyte *data=(*env)->GetByteArrayElements(env,bytes,NULL);
    if(data){int result=klj_setxattr(file,name,data,(size_t)length,flags),error=errno;(*env)->ReleaseByteArrayElements(env,bytes,data,JNI_ABORT);if(result)syscall_error(env,"setxattr",error);}
    free(file);free(name);
}
JNIEXPORT void JNICALL Java_libcore_io_Linux_removexattr(JNIEnv *env,jobject owner,jstring value,jstring key){
    (void)owner;char *file=path(env,value);if(!file)return;char *name=utf8(env,key);if(!name){free(file);return;}
    int result=klj_removexattr(file,name),error=errno;free(file);free(name);if(result)syscall_error(env,"removexattr",error);
}
static jobject stat_value(JNIEnv *env,const struct statvfs *value){
    jclass c=(*env)->FindClass(env,"android/system/StructStatVfs");if(!c)return NULL;
    jmethodID ctor=(*env)->GetMethodID(env,c,"<init>","(JJJJJJJJJJJ)V");
    jobject result=ctor?(*env)->NewObject(env,c,ctor,(jlong)value->f_bsize,(jlong)value->f_frsize,
        (jlong)value->f_blocks,(jlong)value->f_bfree,(jlong)value->f_bavail,(jlong)value->f_files,
        (jlong)value->f_ffree,(jlong)value->f_favail,(jlong)value->f_fsid,
        (jlong)(((value->f_flag&ST_RDONLY)?1:0)|((value->f_flag&ST_NOSUID)?2:0)),(jlong)value->f_namemax):NULL;
    (*env)->DeleteLocalRef(env,c);return result;
}
JNIEXPORT jobject JNICALL Java_libcore_io_Linux_statvfs(JNIEnv *env,jobject owner,jstring value){
    (void)owner;char *mapped=path(env,value);if(!mapped)return NULL;
    struct statvfs stats;int status=statvfs(mapped,&stats),error=errno;free(mapped);
    if(status){syscall_error(env,"statvfs",error);return NULL;}return stat_value(env,&stats);
}
JNIEXPORT jobject JNICALL Java_libcore_io_Linux_fstatvfs(JNIEnv *env,jobject owner,jobject descriptor){
    (void)owner;if(!descriptor){fail(env,"java/lang/NullPointerException","File descriptor is null");return NULL;}
    jclass c=(*env)->FindClass(env,"java/io/FileDescriptor");if(!c)return NULL;
    jfieldID field=(*env)->GetFieldID(env,c,"fd","I");int fd=field?(*env)->GetIntField(env,descriptor,field):-1;(*env)->DeleteLocalRef(env,c);if(!field)return NULL;
    struct statvfs stats;if(fstatvfs(fd,&stats)){syscall_error(env,"fstatvfs",errno);return NULL;}return stat_value(env,&stats);
}
JNIEXPORT jstring JNICALL Java_libcore_io_Linux_strerror(JNIEnv *env,jobject owner,jint value){
    (void)owner;int host=kl_errno_from_linux(value);return text(env,host<0?"Unknown error":strerror(host));
}
/* Preserve existing process environment semantics at the original libcore ABI,
 * using standard UTF-8 rather than JNI's modified UTF-8 encoding. */
static pthread_mutex_t environment_lock=PTHREAD_MUTEX_INITIALIZER;
JNIEXPORT jstring JNICALL Java_libcore_io_Linux_getenv(JNIEnv *env,jobject owner,jstring key){
    (void)owner;char *name=utf8(env,key);if(!name)return NULL;
    pthread_mutex_lock(&environment_lock);const char *value=getenv(name);char *copy=value?strdup(value):NULL;pthread_mutex_unlock(&environment_lock);free(name);
    if(value&&!copy){fail(env,"java/lang/OutOfMemoryError","Environment allocation failed");return NULL;}jstring result=text(env,copy);free(copy);return result;
}
JNIEXPORT void JNICALL Java_libcore_io_Linux_setenv(JNIEnv *env,jobject owner,jstring key,jstring value,jboolean overwrite){
    (void)owner;char *name=utf8(env,key);if(!name)return;char *content=utf8(env,value);if(!content){free(name);return;}
    pthread_mutex_lock(&environment_lock);int status=setenv(name,content,overwrite),error=errno;pthread_mutex_unlock(&environment_lock);free(name);free(content);if(status)syscall_error(env,"setenv",error);
}
JNIEXPORT void JNICALL Java_libcore_io_Linux_unsetenv(JNIEnv *env,jobject owner,jstring key){
    (void)owner;char *name=utf8(env,key);if(!name)return;pthread_mutex_lock(&environment_lock);int status=unsetenv(name),error=errno;pthread_mutex_unlock(&environment_lock);free(name);if(status)syscall_error(env,"unsetenv",error);
}
JNIEXPORT jobjectArray JNICALL Java_libcore_io_Linux_environ(JNIEnv *env,jobject owner){
    (void)owner;size_t count=0;char **copy=NULL;int failed=0;
    pthread_mutex_lock(&environment_lock);char **values=*_NSGetEnviron();while(values[count])count++;
    copy=calloc(count?count:1,sizeof(*copy));if(!copy)failed=1;else for(size_t i=0;i<count;i++){copy[i]=strdup(values[i]);if(!copy[i]){failed=1;break;}}
    pthread_mutex_unlock(&environment_lock);
    jobjectArray result=NULL;if(failed)fail(env,"java/lang/OutOfMemoryError","Environment allocation failed");
    else {jclass c=(*env)->FindClass(env,"java/lang/String");if(c){result=(*env)->NewObjectArray(env,(jsize)count,c,NULL);(*env)->DeleteLocalRef(env,c);if(result)for(size_t i=0;i<count;i++){jstring item=text(env,copy[i]);if(!item)break;(*env)->SetObjectArrayElement(env,result,(jsize)i,item);(*env)->DeleteLocalRef(env,item);if((*env)->ExceptionCheck(env))break;}}}
    if(copy){for(size_t i=0;i<count;i++)free(copy[i]);free(copy);}return result;
}

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <poll.h>
#include <limits.h>
#include <dlfcn.h>
/* These are original libcore native entry points. All descriptor objects carry
 * actual JDK/POSIX descriptors; no Android/ART address or second file store. */
static jfieldID fd_field(JNIEnv *e){jclass c=(*e)->FindClass(e,"java/io/FileDescriptor");if(!c)return NULL;jfieldID f=(*e)->GetFieldID(e,c,"fd","I");(*e)->DeleteLocalRef(e,c);return f;}
static int fd_get(JNIEnv *e,jobject value){if(!value){fail(e,"java/lang/NullPointerException","File descriptor is null");return -1;}jfieldID f=fd_field(e);return f?(*e)->GetIntField(e,value,f):-1;}
static jobject fd_new(JNIEnv *e,int fd){jclass c=(*e)->FindClass(e,"java/io/FileDescriptor");if(!c)return NULL;jmethodID ctor=(*e)->GetMethodID(e,c,"<init>","()V");jobject result=ctor?(*e)->NewObject(e,c,ctor):NULL;jfieldID f=result?(*e)->GetFieldID(e,c,"fd","I"):NULL;if(f)(*e)->SetIntField(e,result,f,fd);(*e)->DeleteLocalRef(e,c);return result;}
static int open_flags(JNIEnv *e,int flags){typedef int(*convert)(int);convert fn=(convert)dlsym(RTLD_DEFAULT,"kl_open_flags");if(!fn){fail(e,"java/lang/IllegalStateException","Klepton open flags are unavailable");return -1;}return fn(flags);}
JNIEXPORT jobject JNICALL Java_libcore_io_Linux_open(JNIEnv *e,jobject self,jstring value,jint flags,jint mode){(void)self;char *p=path(e,value);if(!p)return NULL;int host=open_flags(e,flags);if((*e)->ExceptionCheck(e)){free(p);return NULL;}int fd=open(p,host,mode),code=errno;free(p);if(fd<0){syscall_error(e,"open",code);return NULL;}klj_file_event_fd(fd,0x20);jobject result=fd_new(e,fd);if(!result)klj_file_close(fd);return result;}
JNIEXPORT void JNICALL Java_libcore_io_Linux_close(JNIEnv *e,jobject self,jobject value){(void)self;int fd=fd_get(e,value);if((*e)->ExceptionCheck(e))return;jfieldID field=fd_field(e);if(!field)return;(*e)->SetIntField(e,value,field,-1);if(klj_file_close(fd))syscall_error(e,"close",errno);}
JNIEXPORT jobject JNICALL Java_libcore_io_Linux_dup(JNIEnv *e,jobject self,jobject value){(void)self;int fd=fd_get(e,value);if((*e)->ExceptionCheck(e))return NULL;int copy=dup(fd);if(copy<0){syscall_error(e,"dup",errno);return NULL;}jobject result=fd_new(e,copy);if(!result)close(copy);return result;}
JNIEXPORT jobject JNICALL Java_libcore_io_Linux_dup2(JNIEnv *e,jobject self,jobject value,jint target){(void)self;int fd=fd_get(e,value);if((*e)->ExceptionCheck(e))return NULL;int copy=dup2(fd,target);if(copy<0){syscall_error(e,"dup2",errno);return NULL;}jobject result=fd_new(e,copy);if(!result&&copy!=fd)close(copy);return result;}
#define PATH_MODE(name,fn) JNIEXPORT void JNICALL Java_libcore_io_Linux_##name(JNIEnv*e,jobject s,jstring value,jint mode){(void)s;char*p=path(e,value);if(!p)return;int r=fn(p,mode),c=errno;free(p);if(r)syscall_error(e,#name,c);}
PATH_MODE(chmod,chmod) PATH_MODE(mkdir,mkdir) PATH_MODE(mkfifo,mkfifo)
#define PATH_ONE(name,fn) JNIEXPORT void JNICALL Java_libcore_io_Linux_##name(JNIEnv*e,jobject s,jstring value){(void)s;char*p=path(e,value);if(!p)return;int r=fn(p),c=errno;free(p);if(r)syscall_error(e,#name,c);}
PATH_ONE(unlink,unlink) PATH_ONE(remove,remove)
#define PATH_TWO(name,fn) JNIEXPORT void JNICALL Java_libcore_io_Linux_##name(JNIEnv*e,jobject s,jstring first,jstring second){(void)s;char*a=path(e,first);if(!a)return;char*b=path(e,second);if(!b){free(a);return;}int r=fn(a,b),c=errno;free(a);free(b);if(r)syscall_error(e,#name,c);}
PATH_TWO(rename,rename) PATH_TWO(link,link)
/* Relative symlink contents are data and remain relative to the new link. */
JNIEXPORT void JNICALL Java_libcore_io_Linux_symlink(JNIEnv*e,jobject s,jstring first,jstring second){(void)s;char*a=utf8(e,first);if(!a)return;if(a[0]=='/'){free(a);a=path(e,first);if(!a)return;}char*b=path(e,second);if(!b){free(a);return;}int r=symlink(a,b),c=errno;free(a);free(b);if(r)syscall_error(e,"symlink",c);}
#define PATH_OWNER(name,fn) JNIEXPORT void JNICALL Java_libcore_io_Linux_##name(JNIEnv*e,jobject s,jstring value,jint uid,jint gid){(void)s;char*p=path(e,value);if(!p)return;int r=fn(p,klj_process_uid_to_host(uid),klj_process_gid_to_host(gid)),c=errno;free(p);if(r)syscall_error(e,#name,c);}
PATH_OWNER(chown,chown) PATH_OWNER(lchown,lchown)
JNIEXPORT jboolean JNICALL Java_libcore_io_Linux_access(JNIEnv*e,jobject s,jstring value,jint mode){(void)s;char*p=path(e,value);if(!p)return JNI_FALSE;int r=access(p,mode),c=errno;free(p);if(r){syscall_error(e,"access",c);return JNI_FALSE;}return JNI_TRUE;}
#define FD_OPERATION(name,fn) JNIEXPORT void JNICALL Java_libcore_io_Linux_##name(JNIEnv*e,jobject s,jobject value){(void)s;int fd=fd_get(e,value);if((*e)->ExceptionCheck(e))return;if(fn(fd))syscall_error(e,#name,errno);}
FD_OPERATION(fsync,fsync)
/* Darwin fsync provides the fdatasync durability contract, including metadata. */
FD_OPERATION(fdatasync,fsync)
JNIEXPORT void JNICALL Java_libcore_io_Linux_fchmod(JNIEnv*e,jobject s,jobject v,jint m){(void)s;int fd=fd_get(e,v);if(!(*e)->ExceptionCheck(e)&&fchmod(fd,m))syscall_error(e,"fchmod",errno);}
JNIEXPORT void JNICALL Java_libcore_io_Linux_fchown(JNIEnv*e,jobject s,jobject v,jint u,jint g){(void)s;int fd=fd_get(e,v);if(!(*e)->ExceptionCheck(e)&&fchown(fd,klj_process_uid_to_host(u),klj_process_gid_to_host(g)))syscall_error(e,"fchown",errno);}
JNIEXPORT void JNICALL Java_libcore_io_Linux_ftruncate(JNIEnv*e,jobject s,jobject v,jlong n){(void)s;int fd=fd_get(e,v);if(!(*e)->ExceptionCheck(e)&&ftruncate(fd,n))syscall_error(e,"ftruncate",errno);}
JNIEXPORT jlong JNICALL Java_libcore_io_Linux_lseek(JNIEnv*e,jobject s,jobject v,jlong n,jint whence){(void)s;int fd=fd_get(e,v);if((*e)->ExceptionCheck(e))return -1;off_t r=lseek(fd,n,klj_seek_whence_to_host(whence));if(r<0)syscall_error(e,"lseek",errno);return r;}
JNIEXPORT jboolean JNICALL Java_libcore_io_Linux_isatty(JNIEnv*e,jobject s,jobject v){(void)s;int fd=fd_get(e,v);return (*e)->ExceptionCheck(e)?JNI_FALSE:(jboolean)isatty(fd);}
JNIEXPORT jint JNICALL Java_libcore_io_Linux_umaskImpl(JNIEnv*e,jobject s,jint mask){(void)e;(void)s;return umask(mask);}
static jobject stat_object(JNIEnv*e,const struct stat*v){jclass t=(*e)->FindClass(e,"android/system/StructTimespec");if(!t)return NULL;jmethodID ti=(*e)->GetMethodID(e,t,"<init>","(JJ)V");jobject a=ti?(*e)->NewObject(e,t,ti,(jlong)v->st_atimespec.tv_sec,(jlong)v->st_atimespec.tv_nsec):NULL;
    jobject m=(!(*e)->ExceptionCheck(e)&&ti)?(*e)->NewObject(e,t,ti,(jlong)v->st_mtimespec.tv_sec,(jlong)v->st_mtimespec.tv_nsec):NULL;
    jobject c=(!(*e)->ExceptionCheck(e)&&ti)?(*e)->NewObject(e,t,ti,(jlong)v->st_ctimespec.tv_sec,(jlong)v->st_ctimespec.tv_nsec):NULL;(*e)->DeleteLocalRef(e,t);
    jobject result=NULL;if(a&&m&&c){jclass type=(*e)->FindClass(e,"android/system/StructStat");if(type){jmethodID init=(*e)->GetMethodID(e,type,"<init>","(JJIJIIJJLandroid/system/StructTimespec;Landroid/system/StructTimespec;Landroid/system/StructTimespec;JJ)V");if(init)result=(*e)->NewObject(e,type,init,(jlong)v->st_dev,(jlong)v->st_ino,(jint)v->st_mode,(jlong)v->st_nlink,(jint)klj_process_uid_from_host(v->st_uid),(jint)klj_process_gid_from_host(v->st_gid),(jlong)v->st_rdev,(jlong)v->st_size,a,m,c,(jlong)v->st_blksize,(jlong)v->st_blocks);(*e)->DeleteLocalRef(e,type);}}
    if(a)(*e)->DeleteLocalRef(e,a);if(m)(*e)->DeleteLocalRef(e,m);if(c)(*e)->DeleteLocalRef(e,c);return result;}
#define PATH_STAT(name,fn) JNIEXPORT jobject JNICALL Java_libcore_io_Linux_##name(JNIEnv*e,jobject s,jstring value){(void)s;char*p=path(e,value);if(!p)return NULL;struct stat v;int r=fn(p,&v),c=errno;free(p);if(r){syscall_error(e,#name,c);return NULL;}return stat_object(e,&v);}
PATH_STAT(stat,stat) PATH_STAT(lstat,lstat)
JNIEXPORT jobject JNICALL Java_libcore_io_Linux_fstat(JNIEnv*e,jobject s,jobject value){(void)s;int fd=fd_get(e,value);if((*e)->ExceptionCheck(e))return NULL;struct stat v;if(fstat(fd,&v)){syscall_error(e,"fstat",errno);return NULL;}return stat_object(e,&v);}
JNIEXPORT jstring JNICALL Java_libcore_io_Linux_realpath(JNIEnv*e,jobject s,jstring value){(void)s;char*p=path(e,value);if(!p)return NULL;char*r=realpath(p,NULL);int c=errno;free(p);if(!r){syscall_error(e,"realpath",c);return NULL;}jstring result=text(e,r);free(r);return result;}
JNIEXPORT jstring JNICALL Java_libcore_io_Linux_readlink(JNIEnv*e,jobject s,jstring value){(void)s;char*p=path(e,value);if(!p)return NULL;char buffer[PATH_MAX+1];ssize_t n=readlink(p,buffer,PATH_MAX);int c=errno;free(p);if(n<0){syscall_error(e,"readlink",c);return NULL;}buffer[n]=0;return text(e,buffer);}
/* GetByteArrayElements is not a critical section: blocking I/O never pins the
 * JVM heap against collection. Direct buffers retain their actual native store. */
typedef struct {void* data;jbyteArray array;jbyte* elements;} io_buffer;
static int buffer_get(JNIEnv*e,jobject value,int offset,int count,io_buffer*out){memset(out,0,sizeof(*out));if(!value){fail(e,"java/lang/NullPointerException","I/O buffer is null");return 0;}if(offset<0||count<0){fail(e,"java/lang/IndexOutOfBoundsException","Negative I/O range");return 0;}jclass bytesClass=(*e)->FindClass(e,"[B");if(!bytesClass)return 0;int array=(*e)->IsInstanceOf(e,value,bytesClass);(*e)->DeleteLocalRef(e,bytesClass);jlong capacity=array?(*e)->GetArrayLength(e,value):(*e)->GetDirectBufferCapacity(e,value);if(capacity<0||(jlong)offset+count>capacity){fail(e,"java/lang/IndexOutOfBoundsException","I/O range exceeds buffer");return 0;}if(array){out->array=value;out->elements=(*e)->GetByteArrayElements(e,value,NULL);if(!out->elements)return 0;out->data=out->elements+offset;}else{out->data=(char*)(*e)->GetDirectBufferAddress(e,value)+offset;if(!out->data&&count){fail(e,"java/lang/IllegalArgumentException","I/O buffer has no storage");return 0;}}return 1;}
static void buffer_release(JNIEnv*e,io_buffer*b,int write){if(b->elements)(*e)->ReleaseByteArrayElements(e,b->array,b->elements,write?JNI_ABORT:0);}
/* Reuse the JDK's existing native-thread wake signal for libcore close cancellation. */
typedef struct blocking_io {int fd,cancelled;jlong thread;struct blocking_io *next;} blocking_io;
static pthread_mutex_t blocking_gate=PTHREAD_MUTEX_INITIALIZER;
static blocking_io *blocking_calls;
static jclass native_thread_class;
static jmethodID native_thread_current,native_thread_signal;
static int begin_blocking(JNIEnv*e,jobject descriptor,blocking_io*call){
    memset(call,0,sizeof(*call));pthread_mutex_lock(&blocking_gate);
    if(!native_thread_class){jclass local=(*e)->FindClass(e,"sun/nio/ch/NativeThread");if(local){native_thread_current=(*e)->GetStaticMethodID(e,local,"currentNativeThread","()J");native_thread_signal=native_thread_current?(*e)->GetStaticMethodID(e,local,"signal","(J)V"):NULL;if(native_thread_signal)native_thread_class=(*e)->NewGlobalRef(e,local);(*e)->DeleteLocalRef(e,local);}}
    if(!native_thread_class){pthread_mutex_unlock(&blocking_gate);return 0;}
    call->fd=fd_get(e,descriptor);if((*e)->ExceptionCheck(e)){pthread_mutex_unlock(&blocking_gate);return 0;}
    call->thread=(*e)->CallStaticLongMethod(e,native_thread_class,native_thread_current);
    if((*e)->ExceptionCheck(e)){pthread_mutex_unlock(&blocking_gate);return 0;}
    if(call->fd<0){pthread_mutex_unlock(&blocking_gate);syscall_error(e,"read/write",EBADF);return 0;}
    call->next=blocking_calls;blocking_calls=call;pthread_mutex_unlock(&blocking_gate);return 1;
}
static int end_blocking(blocking_io*call){pthread_mutex_lock(&blocking_gate);blocking_io**entry=&blocking_calls;while(*entry&&*entry!=call)entry=&(*entry)->next;if(*entry)*entry=call->next;int cancelled=call->cancelled;pthread_mutex_unlock(&blocking_gate);return cancelled;}
JNIEXPORT void JNICALL Java_libcore_io_AsynchronousCloseMonitor_signalBlockedThreads(JNIEnv*e,jclass type,jobject descriptor){
    (void)type;int fd=fd_get(e,descriptor);if((*e)->ExceptionCheck(e))return;
    pthread_mutex_lock(&blocking_gate);for(blocking_io*call=blocking_calls;call;call=call->next)if(call->fd==fd){call->cancelled=1;(*e)->CallStaticVoidMethod(e,native_thread_class,native_thread_signal,call->thread);if((*e)->ExceptionCheck(e))break;}pthread_mutex_unlock(&blocking_gate);
}
static jint io(JNIEnv*e,jobject descriptor,jobject value,int offset,int count,jlong position,int positioned,int writing){int fd=fd_get(e,descriptor);if((*e)->ExceptionCheck(e))return -1;io_buffer b;if(!buffer_get(e,value,offset,count,&b))return -1;blocking_io call;int tracked=!positioned&&count>0;if(tracked&&!begin_blocking(e,descriptor,&call)){buffer_release(e,&b,writing);return -1;}ssize_t n=positioned?(writing?pwrite(fd,b.data,count,position):klj_file_pread(fd,b.data,count,position)):(writing?write(fd,b.data,count):klj_file_read(fd,b.data,count));int c=errno;if(tracked&&end_blocking(&call)){n=-1;c=EBADF;}buffer_release(e,&b,writing);if(n<0)syscall_error(e,positioned?(writing?"pwrite":"pread"):(writing?"write":"read"),c);return (jint)n;}
JNIEXPORT jint JNICALL Java_libcore_io_Linux_readBytes(JNIEnv*e,jobject s,jobject fd,jobject v,jint o,jint n){(void)s;return io(e,fd,v,o,n,0,0,0);}
JNIEXPORT jint JNICALL Java_libcore_io_Linux_writeBytes(JNIEnv*e,jobject s,jobject fd,jobject v,jint o,jint n){(void)s;return io(e,fd,v,o,n,0,0,1);}
JNIEXPORT jint JNICALL Java_libcore_io_Linux_preadBytes(JNIEnv*e,jobject s,jobject fd,jobject v,jint o,jint n,jlong p){(void)s;return io(e,fd,v,o,n,p,1,0);}
JNIEXPORT jint JNICALL Java_libcore_io_Linux_pwriteBytes(JNIEnv*e,jobject s,jobject fd,jobject v,jint o,jint n,jlong p){(void)s;return io(e,fd,v,o,n,p,1,1);}
static int fcntl_call(JNIEnv*e,jobject fd,int command,int arg){typedef int(*call)(int,int,uintptr_t);call fn=(call)dlsym(RTLD_DEFAULT,"kl_fcntl");if(!fn){fail(e,"java/lang/IllegalStateException","Klepton descriptor flags are unavailable");return -1;}int number=fd_get(e,fd);if((*e)->ExceptionCheck(e))return -1;int result=fn(number,command,(uintptr_t)arg);if(result<0)syscall_error(e,"fcntl",errno);return result;}
JNIEXPORT jint JNICALL Java_libcore_io_Linux_fcntlInt(JNIEnv*e,jobject s,jobject fd,jint cmd,jint arg){(void)s;return fcntl_call(e,fd,cmd,arg);}
JNIEXPORT jint JNICALL Java_libcore_io_Linux_fcntlVoid(JNIEnv*e,jobject s,jobject fd,jint cmd){(void)s;return fcntl_call(e,fd,cmd,0);}
JNIEXPORT jobjectArray JNICALL Java_libcore_io_Linux_pipe2(JNIEnv*e,jobject s,jint flags){(void)s;if(flags&~(0x800|0x80000)){syscall_error(e,"pipe2",EINVAL);return NULL;}int pair[2];if(pipe(pair)){syscall_error(e,"pipe2",errno);return NULL;}for(int i=0;i<2;i++){if(((flags&0x800)&&fcntl(pair[i],F_SETFL,O_NONBLOCK)<0)||((flags&0x80000)&&fcntl(pair[i],F_SETFD,FD_CLOEXEC)<0)){int c=errno;close(pair[0]);close(pair[1]);syscall_error(e,"pipe2",c);return NULL;}}
    jclass type=(*e)->FindClass(e,"java/io/FileDescriptor");jobjectArray result=type?(*e)->NewObjectArray(e,2,type,NULL):NULL;if(type)(*e)->DeleteLocalRef(e,type);if(result){for(int i=0;i<2;i++){jobject value=fd_new(e,pair[i]);if(!value)break;(*e)->SetObjectArrayElement(e,result,i,value);(*e)->DeleteLocalRef(e,value);if((*e)->ExceptionCheck(e))break;}}if(!result||(*e)->ExceptionCheck(e)){close(pair[0]);close(pair[1]);return NULL;}return result;}
JNIEXPORT jint JNICALL Java_libcore_io_Linux_poll(JNIEnv*e,jobject s,jobjectArray values,jint timeout){(void)s;if(!values){fail(e,"java/lang/NullPointerException","Poll descriptors are null");return -1;}int count=(*e)->GetArrayLength(e,values);struct pollfd* fds=calloc(count?count:1,sizeof(*fds));if(!fds){fail(e,"java/lang/OutOfMemoryError","Poll allocation failed");return -1;}jclass type=(*e)->FindClass(e,"android/system/StructPollfd");jfieldID fd=type?(*e)->GetFieldID(e,type,"fd","Ljava/io/FileDescriptor;"):NULL;jfieldID events=fd?(*e)->GetFieldID(e,type,"events","S"):NULL;jfieldID revents=events?(*e)->GetFieldID(e,type,"revents","S"):NULL;int result=-1;if(revents){for(int i=0;i<count;i++){jobject value=(*e)->GetObjectArrayElement(e,values,i);fds[i].fd=-1;if(!value)continue;jobject descriptor=(*e)->GetObjectField(e,value,fd);if(descriptor){fds[i].fd=fd_get(e,descriptor);(*e)->DeleteLocalRef(e,descriptor);}fds[i].events=(*e)->GetShortField(e,value,events);(*e)->DeleteLocalRef(e,value);if((*e)->ExceptionCheck(e))break;}
    if(!(*e)->ExceptionCheck(e)){result=klj_poll(fds,count,timeout);int c=errno;if(result<0)syscall_error(e,"poll",c);else for(int i=0;i<count;i++){jobject value=(*e)->GetObjectArrayElement(e,values,i);if(value){(*e)->SetShortField(e,value,revents,fds[i].revents);(*e)->DeleteLocalRef(e,value);}}}}
    if(type)(*e)->DeleteLocalRef(e,type);free(fds);return result;}
JNIEXPORT jlong JNICALL Java_libcore_io_Linux_sysconf(JNIEnv*e,jobject s,jint name){(void)s;typedef long(*call)(int);call fn=(call)dlsym(RTLD_DEFAULT,"klb_sysconf");if(!fn){fail(e,"java/lang/IllegalStateException","Klepton system limits are unavailable");return -1;}return fn(name);}
JNIEXPORT jobject JNICALL Java_libcore_io_Linux_uname(JNIEnv*e,jobject s){(void)s;struct utsname v;if(uname(&v)){syscall_error(e,"uname",errno);return NULL;}jclass type=(*e)->FindClass(e,"android/system/StructUtsname");if(!type)return NULL;jmethodID init=(*e)->GetMethodID(e,type,"<init>","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");jobject result=NULL;if(init){jstring a=text(e,v.sysname),b=text(e,v.nodename),c=text(e,v.release),d=text(e,v.version),f=text(e,v.machine);if(a&&b&&c&&d&&f)result=(*e)->NewObject(e,type,init,a,b,c,d,f);if(a)(*e)->DeleteLocalRef(e,a);if(b)(*e)->DeleteLocalRef(e,b);if(c)(*e)->DeleteLocalRef(e,c);if(d)(*e)->DeleteLocalRef(e,d);if(f)(*e)->DeleteLocalRef(e,f);}(*e)->DeleteLocalRef(e,type);return result;}
JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostFileDescriptor_setInt(JNIEnv*e,jclass c,jobject value,jint number){(void)c;if(!value){fail(e,"java/lang/NullPointerException","File descriptor is null");return;}jfieldID field=fd_field(e);if(field)(*e)->SetIntField(e,value,field,number);}
JNIEXPORT jboolean JNICALL Java_maclauncher_android_framework_HostFileDescriptor_isSocket(JNIEnv*e,jclass c,jobject value){(void)c;int fd=fd_get(e,value);if((*e)->ExceptionCheck(e))return JNI_FALSE;struct stat v;return fstat(fd,&v)==0&&S_ISSOCK(v.st_mode);}

#include <sys/socket.h>
JNIEXPORT jlong JNICALL Java_libcore_io_Linux_sendfile(JNIEnv*e,jobject self,jobject output,jobject input,jobject offset,jlong count){
    (void)self;int in=fd_get(e,input);if((*e)->ExceptionCheck(e))return -1;int out=fd_get(e,output);if((*e)->ExceptionCheck(e))return -1;
    if(count<0){syscall_error(e,"sendfile",EINVAL);return -1;}
    struct stat source,destination;if(fstat(in,&source)||fstat(out,&destination)){syscall_error(e,"sendfile",errno);return -1;}
    // Darwin sendfile transfers a regular file into a socket. AOSP FileUtils
    // already falls back to its own Java copy for unsupported file-to-file I/O.
    if(!S_ISREG(source.st_mode)||!S_ISSOCK(destination.st_mode)){syscall_error(e,"sendfile",EINVAL);return -1;}
    jfieldID field=NULL;off_t position;
    if(offset){jclass type=(*e)->GetObjectClass(e,offset);if(!type)return -1;field=(*e)->GetFieldID(e,type,"value","J");(*e)->DeleteLocalRef(e,type);if(!field)return -1;position=(*e)->GetLongField(e,offset,field);}
    else {position=lseek(in,0,SEEK_CUR);if(position<0){syscall_error(e,"sendfile",errno);return -1;}}
    if(position<0||count>INT64_MAX-position){syscall_error(e,"sendfile",EINVAL);return -1;}
    if(count==0)return 0;
    blocking_io call;if(!begin_blocking(e,output,&call))return -1;
    off_t transferred=count;int result=sendfile(in,out,position,&transferred,NULL,0),error=errno;
    if(end_blocking(&call)&&transferred==0){result=-1;error=EBADF;}
    if(transferred>0){if(offset)(*e)->SetLongField(e,offset,field,position+transferred);else if(lseek(in,position+transferred,SEEK_SET)<0){syscall_error(e,"sendfile",errno);return -1;}return transferred;}
    if(result<0){syscall_error(e,"sendfile",error);return -1;}return 0;
}
static int socket_domain(int domain){switch(domain){case 1:return AF_UNIX;case 2:return AF_INET;case 10:return AF_INET6;default:return -1;}}
static int socket_kind(int type){switch(type&0xf){case 1:return SOCK_STREAM;case 2:return SOCK_DGRAM;case 3:return SOCK_RAW;case 5:return SOCK_SEQPACKET;default:return -1;}}
static int socket_flags(int fd,int type){int one=1;setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one));if((type&0x800)&&fcntl(fd,F_SETFL,O_NONBLOCK)<0)return -1;if((type&0x80000)&&fcntl(fd,F_SETFD,FD_CLOEXEC)<0)return -1;return 0;}
JNIEXPORT jobject JNICALL Java_libcore_io_Linux_socket(JNIEnv*e,jobject self,jint domain,jint type,jint protocol){(void)self;int d=socket_domain(domain),t=socket_kind(type);if(d<0||t<0||(type&~0x8080f)){syscall_error(e,"socket",EINVAL);return NULL;}int fd=socket(d,t,protocol);if(fd<0){syscall_error(e,"socket",errno);return NULL;}if(socket_flags(fd,type)){int code=errno;close(fd);syscall_error(e,"socket",code);return NULL;}jobject result=fd_new(e,fd);if(!result)close(fd);return result;}
JNIEXPORT void JNICALL Java_libcore_io_Linux_socketpair(JNIEnv*e,jobject self,jint domain,jint type,jint protocol,jobject first,jobject second){
    (void)self;int d=socket_domain(domain),t=socket_kind(type);if(d<0||t<0||(type&~0x8080f)){syscall_error(e,"socketpair",EINVAL);return;}if(!first||!second||(*e)->IsSameObject(e,first,second)){syscall_error(e,"socketpair",EINVAL);return;}
    int pair[2];if(socketpair(d,t,protocol,pair)){syscall_error(e,"socketpair",errno);return;}if(socket_flags(pair[0],type)||socket_flags(pair[1],type)){int code=errno;close(pair[0]);close(pair[1]);syscall_error(e,"socketpair",code);return;}
    jfieldID field=fd_field(e);if(!field){close(pair[0]);close(pair[1]);return;}(*e)->SetIntField(e,first,field,pair[0]);(*e)->SetIntField(e,second,field,pair[1]);
}
JNIEXPORT void JNICALL Java_libcore_io_Linux_shutdown(JNIEnv*e,jobject self,jobject descriptor,jint how){(void)self;int fd=fd_get(e,descriptor);if((*e)->ExceptionCheck(e))return;if(how<0||how>2){syscall_error(e,"shutdown",EINVAL);return;}if(shutdown(fd,how))syscall_error(e,"shutdown",errno);}
JNIEXPORT jobject JNICALL Java_libcore_io_Linux_getsockoptLinger(JNIEnv*e,jobject self,jobject descriptor,jint level,jint option){
    (void)self;int fd=fd_get(e,descriptor);if((*e)->ExceptionCheck(e))return NULL;if(level!=1||option!=13){syscall_error(e,"getsockopt",ENOPROTOOPT);return NULL;}
    struct linger value;socklen_t size=sizeof(value);if(getsockopt(fd,SOL_SOCKET,SO_LINGER,&value,&size)){syscall_error(e,"getsockopt",errno);return NULL;}
    jclass type=(*e)->FindClass(e,"android/system/StructLinger");if(!type)return NULL;jmethodID ctor=(*e)->GetMethodID(e,type,"<init>","(II)V");jobject result=ctor?(*e)->NewObject(e,type,ctor,value.l_onoff,value.l_linger):NULL;(*e)->DeleteLocalRef(e,type);return result;
}

/* Original libcore mapped-file clients share Klepton's memory ownership and
 * Linux flag translation with guest native libraries. */
#include <sys/mman.h>
#include <stdint.h>
static void *(*mapping_lookup)(const char *);
static pthread_once_t mapping_once=PTHREAD_ONCE_INIT;
static void mapping_init(void){mapping_lookup=(void *(*)(const char *))dlsym(RTLD_DEFAULT,"kl_shim_lookup");}
static void *mapping_symbol(JNIEnv *env,const char *name){
    pthread_once(&mapping_once,mapping_init);void *result=mapping_lookup?mapping_lookup(name):NULL;
    if(!result)fail(env,"java/lang/UnsatisfiedLinkError","Klepton mapped-memory operation is unavailable");return result;
}
static int mapping_length(JNIEnv *env,const char *operation,jlong length){if(length<0){syscall_error(env,operation,EINVAL);return 0;}return 1;}
JNIEXPORT jlong JNICALL Java_libcore_io_Linux_mmap(JNIEnv *env,jobject self,jlong address,jlong length,jint protection,jint flags,jobject descriptor,jlong offset){
    (void)self;if(!mapping_length(env,"mmap",length))return -1;int fd=descriptor?fd_get(env,descriptor):-1;if((*env)->ExceptionCheck(env))return -1;
    void *(*call)(void *,size_t,int,int,int,off_t)=mapping_symbol(env,"mmap");if(!call)return -1;
    void *result=call((void *)(uintptr_t)address,(size_t)length,protection,flags,fd,(off_t)offset);
    if(result==MAP_FAILED){syscall_error(env,"mmap",errno);return -1;}return (jlong)(uintptr_t)result;
}
JNIEXPORT void JNICALL Java_libcore_io_Linux_munmap(JNIEnv *env,jobject self,jlong address,jlong length){
    (void)self;if(!mapping_length(env,"munmap",length))return;int (*call)(void *,size_t)=mapping_symbol(env,"munmap");if(call&&call((void *)(uintptr_t)address,(size_t)length))syscall_error(env,"munmap",errno);
}
JNIEXPORT void JNICALL Java_libcore_io_Linux_madvise(JNIEnv *env,jobject self,jlong address,jlong length,jint advice){
    (void)self;if(!mapping_length(env,"madvise",length))return;int (*call)(void *,size_t,int)=mapping_symbol(env,"madvise");if(call&&call((void *)(uintptr_t)address,(size_t)length,advice))syscall_error(env,"madvise",errno);
}
JNIEXPORT void JNICALL Java_libcore_io_Linux_mlock(JNIEnv *env,jobject self,jlong address,jlong length){
    (void)self;if(mapping_length(env,"mlock",length)&&mlock((void *)(uintptr_t)address,(size_t)length))syscall_error(env,"mlock",errno);
}
JNIEXPORT void JNICALL Java_libcore_io_Linux_munlock(JNIEnv *env,jobject self,jlong address,jlong length){
    (void)self;if(mapping_length(env,"munlock",length)&&munlock((void *)(uintptr_t)address,(size_t)length))syscall_error(env,"munlock",errno);
}
JNIEXPORT void JNICALL Java_libcore_io_Linux_msync(JNIEnv *env,jobject self,jlong address,jlong length,jint flags){
    (void)self;if(!mapping_length(env,"msync",length))return;
    if((flags&~7)||((flags&1)&&(flags&4))){syscall_error(env,"msync",EINVAL);return;}
    int host=((flags&1)?MS_ASYNC:0)|((flags&2)?MS_INVALIDATE:0)|((flags&4)?MS_SYNC:0);
    if(msync((void *)(uintptr_t)address,(size_t)length,host))syscall_error(env,"msync",errno);
}
JNIEXPORT void JNICALL Java_libcore_io_Linux_mincore(JNIEnv *env,jobject self,jlong address,jlong length,jbyteArray vector){
    (void)self;if(!mapping_length(env,"mincore",length))return;
    if(!vector){fail(env,"java/lang/NullPointerException","Memory residency vector is null");return;}
    size_t page=(size_t)getpagesize(),count=(size_t)length/page+((size_t)length%page!=0);
    if(count>(size_t)(*env)->GetArrayLength(env,vector)){fail(env,"java/lang/IllegalArgumentException","Memory residency vector is too short");return;}
    char *result=calloc(count?count:1,1);if(!result){fail(env,"java/lang/OutOfMemoryError","Memory residency allocation failed");return;}
    if(mincore((void *)(uintptr_t)address,(size_t)length,result))syscall_error(env,"mincore",errno);
    else{for(size_t i=0;i<count;i++)result[i]&=1;(*env)->SetByteArrayRegion(env,vector,0,(jsize)count,(jbyte *)result);}free(result);
}

/* Java 21 owns mapped-buffer access, descriptor operations and cleanup. Android's
 * final constructor boolean selects read-only access; the JVM constructor's
 * boolean instead describes a synchronous mapping, so choose the actual read-only
 * class and pass false for that independent JVM field. */
JNIEXPORT jobject JNICALL Java_maclauncher_android_framework_HostMappedBuffer_create(JNIEnv *env,jclass owner,jint capacity,jlong address,jobject descriptor,jobject unmapper,jboolean read_only){
    (void)owner;if(capacity<0){fail(env,"java/lang/IllegalArgumentException","Negative mapped-buffer capacity");return NULL;}
    jclass type=(*env)->FindClass(env,read_only?"java/nio/DirectByteBufferR":"java/nio/DirectByteBuffer");if(!type)return NULL;
    jmethodID constructor=(*env)->GetMethodID(env,type,"<init>","(IJLjava/io/FileDescriptor;Ljava/lang/Runnable;ZLjava/lang/foreign/MemorySegment;)V");
    jobject result=constructor?(*env)->NewObject(env,type,constructor,capacity,address,descriptor,unmapper,JNI_FALSE,NULL):NULL;
    (*env)->DeleteLocalRef(env,type);return result;
}
