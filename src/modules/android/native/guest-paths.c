#include "guest-paths.h"
#include <jni.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {char *prefix,*target;size_t prefix_length,target_length;int identity;} Mapping;
static Mapping *mappings;
static size_t mapping_count;
static pthread_rwlock_t mapping_lock=PTHREAD_RWLOCK_INITIALIZER;

static void release_mappings(Mapping *values,size_t count){
    for(size_t i=0;i<count;++i){free(values[i].prefix);free(values[i].target);}free(values);
}
static void throw_path_error(JNIEnv *env,const char *type,const char *message){
    if((*env)->ExceptionCheck(env))return;
    jclass error=(*env)->FindClass(env,type);if(error){(*env)->ThrowNew(env,error,message);(*env)->DeleteLocalRef(env,error);}
}
static char *copy_path(JNIEnv *env,jobjectArray paths,jsize index){
    jbyteArray bytes=(*env)->GetObjectArrayElement(env,paths,index);if(!bytes)return NULL;
    jsize length=(*env)->GetArrayLength(env,bytes);char *value=malloc((size_t)length+1);
    if(!value)throw_path_error(env,"java/lang/OutOfMemoryError","Cannot allocate Android path mapping");
    if(value){(*env)->GetByteArrayRegion(env,bytes,0,length,(jbyte*)value);value[length]=0;
        if((*env)->ExceptionCheck(env)||!length||value[0]!='/'||memchr(value,0,(size_t)length)){free(value);value=NULL;}}
    (*env)->DeleteLocalRef(env,bytes);return value;
}
JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostFilesystem_installNativePaths(JNIEnv *env,jclass owner,jobjectArray prefixes,jobjectArray targets){
    (void)owner;
    if(!prefixes||!targets||(*env)->GetArrayLength(env,prefixes)!=(*env)->GetArrayLength(env,targets)){
        throw_path_error(env,"java/lang/IllegalArgumentException","Android path mappings must have matching endpoints");return;
    }
    size_t count=(size_t)(*env)->GetArrayLength(env,prefixes);
    Mapping *next=count?calloc(count,sizeof *next):NULL;
    if(count&&!next){throw_path_error(env,"java/lang/OutOfMemoryError","Cannot allocate Android path mappings");return;}
    for(size_t i=0;i<count;++i){
        next[i].prefix=copy_path(env,prefixes,(jsize)i);if(next[i].prefix)next[i].target=copy_path(env,targets,(jsize)i);
        if(!next[i].prefix||!next[i].target){release_mappings(next,count);throw_path_error(env,"java/lang/IllegalArgumentException","Android path mappings require absolute UTF-8 paths");return;}
        next[i].prefix_length=strlen(next[i].prefix);next[i].target_length=strlen(next[i].target);
        next[i].identity=!strcmp(next[i].prefix,next[i].target);
    }
    // Publish all aliases together. Readers finish copying before old storage
    // is retired, including when a fixture or host refreshes the configuration.
    pthread_rwlock_wrlock(&mapping_lock);Mapping *previous=mappings;size_t previous_count=mapping_count;
    mappings=next;mapping_count=count;pthread_rwlock_unlock(&mapping_lock);
    release_mappings(previous,previous_count);
}

// Java Path.normalize and the native resolver must pick the same mapping when
// an application uses dot components. Ordinary asset paths need no allocation.
static char *normalized_path(const char *path){
    if(!strstr(path,"/.")&&!strstr(path,"//"))return NULL;
    size_t length=strlen(path);char *result=malloc(length+1);if(!result)return NULL;
    size_t used=1,at=1;result[0]='/';
    while(at<length){
        while(at<length&&path[at]=='/')++at;
        size_t start=at;while(at<length&&path[at]!='/')++at;
        size_t size=at-start;if(!size||(size==1&&path[start]=='.'))continue;
        if(size==2&&path[start]=='.'&&path[start+1]=='.'){
            while(used>1&&result[used-1]!='/')--used;if(used>1)--used;continue;
        }
        if(used>1)result[used++]='/';memcpy(result+used,path+start,size);used+=size;
    }
    result[used]=0;return result;
}
const char *klj_mapped_guest_path(const char *path,char *buffer,size_t capacity){
    if(!path||path[0]!='/')return NULL;
    char *normalized=normalized_path(path);const char *input=normalized?normalized:path;
    const char *result=NULL;size_t input_length=strlen(input);
    pthread_rwlock_rdlock(&mapping_lock);
    for(size_t i=0;i<mapping_count;++i){
        const Mapping *mapping=&mappings[i];size_t prefix=mapping->prefix_length;
        if(input_length<prefix||memcmp(input,mapping->prefix,prefix)||(input[prefix]&&input[prefix]!='/'))continue;
        size_t suffix=input_length-prefix;
        if(mapping->target_length<capacity&&suffix<capacity-mapping->target_length){
            memcpy(buffer,mapping->target,mapping->target_length);memcpy(buffer+mapping->target_length,input+prefix,suffix+1);result=buffer;
        }else{
            // Never turn an overlong path into a different, truncated filename.
            // Native callers supply the host's PATH_MAX; the host rejects paths
            // exceeding that capacity itself.
            errno=ENAMETOOLONG;result=path;
        }
        break;
    }
    pthread_rwlock_unlock(&mapping_lock);free(normalized);return result;
}
static const char *reverse_mappings(const Mapping *values,size_t count,const char *path,char *buffer,size_t capacity){
    if(!path||path[0]!='/'||!buffer||!capacity)return NULL;
    size_t length=strlen(path),best=0;const Mapping *found=NULL;
    for(size_t i=0;i<count;i++){
        const Mapping *mapping=&values[i];size_t target=mapping->target_length;
        if(length<target||memcmp(path,mapping->target,target)||(path[target]&&path[target]!='/'))continue;
        /* A real guest alias takes precedence over its physical identity alias
         * when both describe the same enrolled immutable file. */
        if(found&&(target<best||(target==best&&!found->identity)))continue;
        found=mapping;best=target;
    }
    const char *result=NULL;
    if(found){
        size_t suffix=length-best;
        if(found->prefix_length<capacity&&suffix<capacity-found->prefix_length){
            memcpy(buffer,found->prefix,found->prefix_length);
            memcpy(buffer+found->prefix_length,path+best,suffix+1);result=buffer;
        }else errno=ENAMETOOLONG;
    }
    return result;
}
const char *klj_guest_path_of_host(const char *path,char *buffer,size_t capacity){
    pthread_rwlock_rdlock(&mapping_lock);
    const char *result=reverse_mappings(mappings,mapping_count,path,buffer,capacity);
    pthread_rwlock_unlock(&mapping_lock);return result;
}
struct KljGuestPathView {Mapping *values;size_t count;};
KljGuestPathView *klj_guest_path_view(void){
    KljGuestPathView *view=calloc(1,sizeof *view);if(!view)return NULL;
    pthread_rwlock_rdlock(&mapping_lock);view->count=mapping_count;
    view->values=view->count?calloc(view->count,sizeof(Mapping)):NULL;
    int ok=!view->count||view->values;
    for(size_t i=0;ok&&i<view->count;i++){
        view->values[i]=mappings[i];
        view->values[i].prefix=strdup(mappings[i].prefix);
        view->values[i].target=strdup(mappings[i].target);
        ok=view->values[i].prefix&&view->values[i].target;
    }
    pthread_rwlock_unlock(&mapping_lock);
    if(!ok){klj_guest_path_view_release(view);return NULL;}
    for(size_t i=0;i<view->count;i++){
        char *canonical=realpath(view->values[i].target,NULL);
        if(canonical){free(view->values[i].target);view->values[i].target=canonical;view->values[i].target_length=strlen(canonical);}
    }
    return view;
}
const char *klj_guest_path_view_name(const KljGuestPathView *view,const char *path,char *buffer,size_t capacity){
    return view?reverse_mappings(view->values,view->count,path,buffer,capacity):NULL;
}
void klj_guest_path_view_release(KljGuestPathView *view){
    if(view){release_mappings(view->values,view->values?view->count:0);free(view);}
}
