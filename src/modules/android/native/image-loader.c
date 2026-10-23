#include "image-loader.h"
#include "native-image-provider.h"
#include "jni-provider.h"
#include "call-frame.h"
#include "jni-registration.h"
#include "jni-invocation.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

typedef struct Image Image;
typedef struct {size_t count;Image *images[];} Dependencies;
typedef struct {
    KljBionicRuntime *runtime;JavaVM *vm;
    pthread_mutex_t mutex;Image *images;
} Images;
struct Image {
    Images *owner;Image *next;void *original;KljJniImage *jni;
    jclass defining_entry;uintptr_t bias;_Atomic int state;
    _Atomic(KljRegistrationTransaction *) exports;
    _Atomic(Dependencies *) dependencies;
};
enum {PROVISIONAL=1,CONSTRUCTED=2,REJECTED=3};
typedef struct {
    Images *owner;JNIEnv *env;jclass entry;jobject preparation;
    Image *root;Image **created;size_t count,capacity;int prepared;
    KljRegistrationTransaction *transaction,*previous_transaction;int partial;
} Preparation;
static _Atomic(Images *) process_images;

static void fail(JNIEnv *env,const char *message){
    if(!env||(*env)->ExceptionCheck(env))return;
    jclass type=(*env)->FindClass(env,"java/lang/UnsatisfiedLinkError");
    if(type){(*env)->ThrowNew(env,type,message);(*env)->DeleteLocalRef(env,type);}
}
static JNIEnv *environment(Images *owner){JNIEnv *env=NULL;return (*owner->vm)->GetEnv(owner->vm,(void **)&env,JNI_VERSION_1_6)==JNI_OK?env:NULL;}
static Image *handle(Images *owner,jlong raw){
    pthread_mutex_lock(&owner->mutex);Image *found=NULL;
    for(Image *image=owner->images;image;image=image->next)if((uintptr_t)image==(uintptr_t)raw){found=image;break;}
    pthread_mutex_unlock(&owner->mutex);return found;
}
static int owns(void *raw,jlong value){return handle(raw,value)!=NULL;}
static Image *target_image(Images *owner,const void *target){const void *identity=klj_jni_provider_identity(target);return identity?handle(owner,(jlong)(uintptr_t)identity):NULL;}
static void *factory(void *raw,void *target,const char *descriptor,char *error,size_t capacity){Images *owner=raw;Image *image=target_image(owner,target);if(!image||atomic_load_explicit(&image->state,memory_order_acquire)==REJECTED){snprintf(error,capacity,"Native library is no longer available");return NULL;}return klj_bionic_native_abi(target,descriptor,error,capacity);}
static int executable(const MaclauncherDlopenImageV1 *image){
    const KljJniProgramHeader *phdr=image->phdr;
    if(image->size!=sizeof(*image)||!image->handle||!phdr||!image->phnum||image->phnum>65536)return -1;
    for(size_t i=0;i<image->phnum;i++)if(phdr[i].type==1&&(phdr[i].flags&1)&&phdr[i].memsz)return 1;
    return 0;
}
static Image *publish(Preparation *load,const MaclauncherDlopenImageV1 *source,char *error,size_t capacity){
    Images *owner=load->owner;Image *image=NULL;int code=executable(source);
    if(code<0){snprintf(error,capacity,"Invalid original native image metadata");return NULL;}
    pthread_mutex_lock(&owner->mutex);
    for(Image *item=owner->images;item;item=item->next)if(item->original==source->handle&&item->bias==source->load_bias&&atomic_load_explicit(&item->state,memory_order_acquire)!=REJECTED){image=item;break;}
    pthread_mutex_unlock(&owner->mutex);
    if(image)return image;
    if(load->count>=load->capacity){snprintf(error,capacity,"Native image graph changed during preparation");return NULL;}
    image=calloc(1,sizeof(*image));if(!image){snprintf(error,capacity,"Native image metadata could not be retained");return NULL;}
    image->owner=owner;image->original=source->handle;image->bias=source->load_bias;
    image->defining_entry=(*load->env)->NewGlobalRef(load->env,load->entry);
    if(!image->defining_entry){free(image);return NULL;}
    atomic_init(&image->state,(source->flags&MACLAUNCHER_DLOPEN_IMAGE_CONSTRUCTED)?CONSTRUCTED:PROVISIONAL);
    atomic_init(&image->exports,NULL);
    atomic_init(&image->dependencies,NULL);
    /* The provider's own record is a VM-lifetime opaque identity. It is never
     * cast to the legacy loader's image or to an original soinfo structure. */
    pthread_mutex_lock(&owner->mutex);image->next=owner->images;owner->images=image;pthread_mutex_unlock(&owner->mutex);
    if(code){
        image->jni=klj_jni_image_publish(load->env,image,source->load_bias,source->phdr,source->phnum,load->entry,factory,owner);
        if(!image->jni){atomic_store_explicit(&image->state,REJECTED,memory_order_release);snprintf(error,capacity,"Native image ownership could not be published");return NULL;}
    }
    load->created[load->count++]=image;return image;
}
static int same_dependencies(const Dependencies *first,const Dependencies *second){return first->count==second->count&&!memcmp(first->images,second->images,first->count*sizeof(*first->images));}
/* Copy actual DT_NEEDED identities while the original linker's borrowed graph
 * is valid. Edges are immutable through VM lifetime, including non-PF_X nodes.
 * A later graph for the same original object must agree with its prior edges. */
static int retain_dependencies(Preparation *load,const MaclauncherDlopenGraphV1 *graph,Image **resolved,char *error,size_t capacity){
    Dependencies **pending=calloc(graph->image_count,sizeof(*pending));if(!pending)goto allocation;
    int ok=0;
    for(size_t i=0;i<graph->image_count;i++){
        const MaclauncherDlopenImageV1 *source=&graph->images[i];size_t count=source->dependency_count;
        if(count>graph->image_count||(count&&!source->dependencies)||count>(SIZE_MAX-sizeof(Dependencies))/sizeof(Image *))goto invalid;
        Dependencies *edges=malloc(sizeof(*edges)+count*sizeof(*edges->images));if(!edges)goto allocation_pending;
        pending[i]=edges;edges->count=count;
        for(size_t j=0;j<count;j++){size_t index=source->dependencies[j];if(index>=graph->image_count||!resolved[index])goto invalid;edges->images[j]=resolved[index];}
        for(size_t j=0;j<i;j++)if(resolved[j]==resolved[i]&&!same_dependencies(pending[j],edges))goto invalid;
    }
    pthread_mutex_lock(&load->owner->mutex);
    for(size_t i=0;i<graph->image_count;i++){
        Dependencies *old=atomic_load_explicit(&resolved[i]->dependencies,memory_order_acquire);
        if(old&&!same_dependencies(old,pending[i])){pthread_mutex_unlock(&load->owner->mutex);goto invalid;}
    }
    for(size_t i=0;i<graph->image_count;i++)if(!atomic_load_explicit(&resolved[i]->dependencies,memory_order_relaxed)){
        atomic_store_explicit(&resolved[i]->dependencies,pending[i],memory_order_release);pending[i]=NULL;
    }
    pthread_mutex_unlock(&load->owner->mutex);ok=1;goto done;
invalid:snprintf(error,capacity,"Invalid original native dependency graph");goto done;
allocation_pending:snprintf(error,capacity,"Native dependency identities could not be retained");
done:for(size_t i=0;i<graph->image_count;i++)free(pending[i]);free(pending);return ok;
allocation:snprintf(error,capacity,"Native dependency identities could not be retained");return 0;
}
static int reachable(Image *anchor,Image *target){
    if(anchor==target)return 1;
    Image *small[16]={anchor},**visited=small;size_t count=1,capacity=16;int result=0;
    for(size_t next=0;next<count;next++){
        Dependencies *edges=atomic_load_explicit(&visited[next]->dependencies,memory_order_acquire);if(!edges)continue;
        for(size_t j=0;j<edges->count;j++){
            Image *image=edges->images[j];if(atomic_load_explicit(&image->state,memory_order_acquire)==REJECTED)continue;
            if(image==target){result=1;goto done;}
            size_t k=0;while(k<count&&visited[k]!=image)k++;if(k<count)continue;
            if(count==capacity){
                if(capacity>SIZE_MAX/(2*sizeof(*visited))){result=-ENOMEM;goto done;}
                Image **larger=malloc(capacity*2*sizeof(*larger));if(!larger){result=-ENOMEM;goto done;}
                memcpy(larger,visited,count*sizeof(*larger));if(visited!=small)free(visited);visited=larger;capacity*=2;
            }
            visited[count++]=image;
        }
    }
done:if(visited!=small)free(visited);return result;
}
static void abort_preparation(Preparation *load){
    if(load->transaction&&!load->partial){klj_registration_txn_discard(load->env,load->transaction);if(load->root)atomic_store_explicit(&load->root->exports,load->previous_transaction,memory_order_release);}
    if(load->partial){for(size_t i=0;i<load->count;i++)atomic_store_explicit(&load->created[i]->state,REJECTED,memory_order_release);return;}
    for(size_t i=load->count;i>0;i--){Image *image=load->created[i-1];klj_jni_image_abort(image->jni);atomic_store_explicit(&image->state,REJECTED,memory_order_release);}
}
static Image *original_image(Images *owner,void *handle,uintptr_t bias){
 Image *found=NULL;pthread_mutex_lock(&owner->mutex);
 for(Image *item=owner->images;item;item=item->next)if(item->original==handle&&item->bias==bias&&atomic_load_explicit(&item->state,memory_order_acquire)!=REJECTED){found=item;break;}
 pthread_mutex_unlock(&owner->mutex);return found;
}
static int observed(Images *owner,KljBionicRuntime *runtime,JNIEnv *env,const KljBionicLinkerEvent *event,char *error,size_t capacity){
 if(owner->runtime!=runtime)return 0;
 if(event->phase==MACLAUNCHER_DLOPEN_RETIRE){
  Image *image=original_image(owner,event->handle,event->bias);
  if(image){klj_jni_image_abort(image->jni);atomic_store_explicit(&image->state,REJECTED,memory_order_release);}return 1;
 }
 Image *anchor=target_image(owner,event->caller);const MaclauncherDlopenGraphV1 *graph=event->graph;
 if(!anchor||atomic_load_explicit(&anchor->state,memory_order_acquire)==REJECTED||!graph||graph->size!=sizeof(*graph)||graph->abi!=MACLAUNCHER_DLOPEN_PREPARE_ABI||!graph->images||!graph->image_count||graph->image_count>65536||graph->root_index>=graph->image_count){if(error&&capacity)snprintf(error,capacity,"Native dlopen caller has no active application image owner");return 0;}
 if(event->phase==MACLAUNCHER_DLOPEN_COMPLETE){
  for(size_t i=0;i<graph->image_count;i++)if(graph->images[i].flags&MACLAUNCHER_DLOPEN_IMAGE_CONSTRUCTED){Image *image=original_image(owner,graph->images[i].handle,graph->images[i].load_bias);if(image)atomic_store_explicit(&image->state,CONSTRUCTED,memory_order_release);}return 1;
 }
 if(!env){
  /* Pure C dlopen does not attach its original Android pthread to Java. Only
   * this authenticated metadata publication needs JNI global references;
   * release that internal attachment before the original linker executes any
   * constructors, and preserve attachment when the caller already owns it. */
  KljJniBorrowedAttachment attachment;
  if(klj_jni_borrow_attachment(owner->vm,&attachment)!=JNI_OK||!attachment.environment){
   if(error&&capacity)snprintf(error,capacity,"Native library metadata service is unavailable");return 0;
  }
  JNIEnv *publication=attachment.environment;
  int result=observed(owner,runtime,publication,event,error,capacity);
  if(attachment.borrowed){
   if((*publication)->ExceptionCheck(publication)){
    (*publication)->ExceptionClear(publication);result=0;
    if(error&&capacity)snprintf(error,capacity,"Native library metadata could not be retained");
   }
  }
  if(klj_jni_return_attachment(&attachment)!=JNI_OK){
   if(error&&capacity)snprintf(error,capacity,"Native library metadata attachment could not be released");return 0;
  }
  return result;
 }
 Preparation load={.owner=owner,.env=env,.entry=anchor->defining_entry,.prepared=1,.capacity=graph->image_count};
 load.created=calloc(load.capacity,sizeof(*load.created));Image **resolved=calloc(graph->image_count,sizeof(*resolved));
 if(!load.created||!resolved){if(error&&capacity)snprintf(error,capacity,"Native dlopen graph could not be retained");goto reject;}
 for(size_t i=0;i<graph->image_count;i++){
  Image *image=publish(&load,&graph->images[i],error,capacity);if(!image)goto reject;
  resolved[i]=image;if(i==graph->root_index)load.root=image;
 }
 if(!load.root||!(*env)->IsSameObject(env,load.root->defining_entry,anchor->defining_entry)){if(error&&capacity)snprintf(error,capacity,"Native dlopen belongs to another application class loader");goto reject;}
 if(!retain_dependencies(&load,graph,resolved,error,capacity))goto reject;
 free(resolved);free(load.created);return 1;
reject:abort_preparation(&load);free(resolved);free(load.created);return 0;
}
static int prepared(void *raw,KljBionicRuntime *runtime,JNIEnv *env,void *request,const MaclauncherDlopenGraphV1 *graph,char *error,size_t capacity,const KljBionicLinkerEvent *event){
    if(event)return observed(raw,runtime,env,event,error,capacity);
    Images *owner=raw;Preparation *load=request;
    if(!load||load->owner!=owner||owner->runtime!=runtime||load->env!=env||load->prepared||!graph||graph->size!=sizeof(*graph)||graph->abi!=MACLAUNCHER_DLOPEN_PREPARE_ABI||!graph->images||!graph->image_count||graph->image_count>65536||graph->root_index>=graph->image_count){snprintf(error,capacity,"Invalid original native preparation transaction");return 0;}
    Image **resolved=NULL;load->prepared=1;load->capacity=graph->image_count+1;
    load->created=calloc(load->capacity,sizeof(*load->created));if(!load->created){snprintf(error,capacity,"Native image graph could not be retained");return 0;}
    resolved=calloc(graph->image_count,sizeof(*resolved));if(!resolved){snprintf(error,capacity,"Native dependency identities could not be retained");goto reject;}
    const KljBionicGuest *guest=klj_bionic_runtime_guest(runtime);
    if(!guest||!publish(load,&guest->helper,error,capacity))goto reject;
    for(size_t i=0;i<graph->image_count;i++){
        const MaclauncherDlopenImageV1 *source=&graph->images[i];int code=executable(source);
        if(code<0){snprintf(error,capacity,"Invalid original native image metadata");goto reject;}
        Image *image=publish(load,source,error,capacity);if(!image)goto reject;
        resolved[i]=image;
        if(i==graph->root_index)load->root=image;
    }
    if(!retain_dependencies(load,graph,resolved,error,capacity))goto reject;
    free(resolved);resolved=NULL;
    if(!load->root||!(*env)->IsSameObject(env,load->root->defining_entry,load->entry)){snprintf(error,capacity,"Native library belongs to another application class loader");goto reject;}
    load->transaction=klj_registration_txn_begin(env);if(!load->transaction)goto reject;
    load->previous_transaction=atomic_exchange_explicit(&load->root->exports,load->transaction,memory_order_acq_rel);
    if(load->preparation&&!klj_native_image_prepare(env,load->preparation,(jlong)(uintptr_t)load->root))goto reject;
    jint committed=klj_registration_txn_commit(env,load->transaction);
    if(committed!=JNI_OK){load->partial=committed==KLJ_REGISTRATION_COMMIT_PARTIAL;if(!(*env)->ExceptionCheck(env))snprintf(error,capacity,"Native exported methods could not be published");goto reject;}
    return 1;
reject:
    free(resolved);abort_preparation(load);return 0;
}
static void *symbol(void *raw,jlong value,const char *name,char *error,size_t capacity){
    Images *owner=raw;Image *image=handle(owner,value);JNIEnv *env=environment(owner);
    if(!image||!env||atomic_load_explicit(&image->state,memory_order_acquire)==REJECTED){if(error&&capacity)snprintf(error,capacity,"Native library is no longer available");return NULL;}
    KljBionicSymbol request={.size=sizeof request,.version=KLJ_BIONIC_RUNTIME_ABI,.handle=image->original,.name=name,.error=error,.error_capacity=capacity};
    int64_t status=klj_bionic_runtime_symbol(owner->runtime,env,&request);
    if(status&&status!=-ENOENT&&!(*env)->ExceptionCheck(env))fail(env,error&&capacity&&*error?error:"Native symbol lookup failed");
    return status?NULL:request.symbol;
}
static void *native_abi(void *raw,jlong value,void *target,const char *descriptor,char *error,size_t capacity){
    Images *owner=raw;Image *image=handle(owner,value);
    if(!image||atomic_load_explicit(&image->state,memory_order_acquire)==REJECTED){snprintf(error,capacity,"Native library is no longer available");return NULL;}
    return klj_jni_provider_native_abi(target,descriptor,error,capacity);
}
static jint register_exports(void *raw,JNIEnv *env,jlong value,jclass target,const JNINativeMethod *methods,jint count){
    Image *image=handle(raw,value);KljRegistrationTransaction *transaction=image?atomic_load_explicit(&image->exports,memory_order_acquire):NULL;
    if(!image||!transaction||atomic_load_explicit(&image->state,memory_order_acquire)==REJECTED){fail(env,"Native exported methods have no active image owner");return JNI_ERR;}
    return klj_registration_txn_stage_exports(env,transaction,target,methods,count);
}
static int initialize(void *raw,JNIEnv *env,jlong value){
    Images *owner=raw;char error[512]={0};void *entry=symbol(owner,value,"JNI_OnLoad",error,sizeof error);
    if((*env)->ExceptionCheck(env))return 0;if(!entry)return 1;
    KljBionicJniOwner *jni=klj_bionic_runtime_jni_owner(owner->runtime);JavaVM *vm=jni?klj_bionic_jni_vm(jni):NULL;
    if(!vm){fail(env,"Native Java VM representation is unavailable");return 0;}
    KljCallPacket packet={.size=sizeof packet,.version=KLJ_CALL_PACKET_VERSION,.target=(uintptr_t)entry};packet.x[0]=(uintptr_t)vm;
    int64_t status=klj_bionic_runtime_packet(owner->runtime,env,&packet);
    void *compatible=NULL;jint version=(jint)packet.result_x[0];
    if(status||packet.status||version<0||(*owner->vm)->GetEnv(owner->vm,&compatible,version)!=JNI_OK){fail(env,"Guest JNI_OnLoad failed");return 0;}
    return !(*env)->ExceptionCheck(env);
}
static int construct(void *raw,JNIEnv *env,jlong value){(void)env;Image *image=handle(raw,value);return image&&atomic_load_explicit(&image->state,memory_order_acquire)==CONSTRUCTED;}
static void reject(void *raw,jlong value){Image *image=handle(raw,value);if(image)atomic_store_explicit(&image->state,REJECTED,memory_order_release);}
static jlong load_image(void *raw,JNIEnv *env,jclass entry,const char *filename,const char *soname,jobject callback,jboolean onload){
    (void)soname;Images *owner=raw;const KljBionicGuest *guest=klj_bionic_runtime_guest(owner->runtime);if(!guest){fail(env,"Native execution is not initialized");return 0;}
    Preparation load={.owner=owner,.env=env,.entry=entry,.preparation=callback};char error[512]={0};
    /* NativeBinding retains application code for the entire JVM lifetime.
     * Original NODELETE also keeps unexpected foreign-registration side effects
     * from referring to unmapped code after a failed preparation transaction. */
    KljBionicOpen request={.size=sizeof request,.version=KLJ_BIONIC_RUNTIME_ABI,.filename=filename,.flags=2|0x1000,.caller=(void *)(uintptr_t)guest->open_prepared,.context=&load,.error=error,.error_capacity=sizeof error};
    int64_t status=klj_bionic_runtime_open(owner->runtime,env,&request);
    if(status||!request.handle||!load.prepared||!load.root||request.handle!=load.root->original){
        /* A rejected prepare already retired provisional ranges before the
         * original linker released its reference. Do not invoke constructors
         * or close a borrowed/rejected original handle a second time. */
        free(load.created);fail(env,*error?error:"Native library preparation failed");return 0;
    }
    for(size_t i=0;i<load.count;i++)atomic_store_explicit(&load.created[i]->state,CONSTRUCTED,memory_order_release);
    free(load.created);jlong result=(jlong)(uintptr_t)load.root;
    if(onload&&!initialize(owner,env,result))return 0;
    return result;
}
/* This is deliberately separate from the JNI wrapper below. NativeActivity,
 * looper/media callbacks and allocators have no leading JNIEnv argument. */
static int64_t invoke_callback(void *raw,JNIEnv *env,jlong value,KljCallPacket *packet){
    Images *owner=raw;
    if(environment(owner)!=env)return -EPERM;
    Image *anchor=handle(owner,value);
    if(!anchor||atomic_load_explicit(&anchor->state,memory_order_acquire)==REJECTED)return -EINVAL;
    if(!packet||packet->size!=sizeof(*packet)||packet->version!=KLJ_CALL_PACKET_VERSION||!packet->target||packet->reserved||packet->argument_padding||packet->stack_bytes>KLJ_PACKET_MAX_STACK||(packet->stack_bytes&&(!packet->stack||(packet->stack_bytes&15))))return -EINVAL;
    Image *target=target_image(owner,(void *)(uintptr_t)packet->target);
    if(!target||atomic_load_explicit(&target->state,memory_order_acquire)==REJECTED)return -EINVAL;
    int allowed=reachable(anchor,target);if(allowed<=0)return allowed?allowed:-EACCES;
    if(env&&(*env)->ExceptionCheck(env))return -ECANCELED;
    int64_t status=env?klj_bionic_runtime_packet(owner->runtime,env,packet):klj_bionic_runtime_native_packet(owner->runtime,packet);
    return status?status:packet->status;
}
static int64_t callback_context(void *raw,JNIEnv *env,jlong value,JNIEnv **guest_env,JavaVM **guest_vm){
    Images *owner=raw;Image *image=handle(owner,value);
    if(!env||environment(owner)!=env||!image||atomic_load_explicit(&image->state,memory_order_acquire)==REJECTED||!guest_env||!guest_vm)return -EINVAL;
    if((*env)->ExceptionCheck(env))return -ECANCELED;
    KljBionicJniOwner *jni=klj_bionic_runtime_jni_owner(owner->runtime);
    JNIEnv *outer=jni?klj_bionic_jni_environment(jni,env):NULL;JavaVM *vm=jni?klj_bionic_jni_vm(jni):NULL;
    if(!outer||!vm)return -ENOMEM;*guest_env=outer;*guest_vm=vm;return 0;
}
int64_t klj_bionic_call_frame_invoke(KljCallPacket *packet){
    Images *owner=atomic_load_explicit(&process_images,memory_order_acquire);
    JNIEnv *real=owner?environment(owner):NULL;
    Image *image=owner&&packet?target_image(owner,(void *)(uintptr_t)packet->target):NULL;
    if(!packet||!image||atomic_load_explicit(&image->state,memory_order_acquire)==REJECTED){if(real)fail(real,"Native invocation has no active image owner");return -EINVAL;}
    JNIEnv *env=(JNIEnv *)(uintptr_t)packet->x[0];
    if(!real||env!=real){if(real)fail(real,"Native JNI entry has a different thread owner");return -EPERM;}
    KljBionicJniOwner *jni=klj_bionic_runtime_jni_owner(owner->runtime);JNIEnv *outer=jni?klj_bionic_jni_environment(jni,real):NULL;
    if(!outer){fail(real,"Native JNI environment is unavailable");return -ENOMEM;}
    packet->x[0]=(uintptr_t)outer;int64_t status=klj_bionic_runtime_packet(owner->runtime,real,packet);packet->x[0]=(uintptr_t)real;
    if(status||packet->status)fail(real,"Android native invocation failed");return status?status:packet->status;
}
int64_t klj_bionic_images_install(KljBionicRuntime *runtime,JNIEnv *env){
    if(!runtime||!env||atomic_load_explicit(&process_images,memory_order_acquire))return -EINVAL;
    Images *owner=calloc(1,sizeof(*owner));if(!owner)return -ENOMEM;owner->runtime=runtime;
    if((*env)->GetJavaVM(env,&owner->vm)!=JNI_OK||pthread_mutex_init(&owner->mutex,NULL)){free(owner);return -EINVAL;}
    int64_t status=klj_bionic_runtime_prepared_callback(runtime,prepared,owner);if(status)return status;
    KljNativeImageProvider provider={.size=sizeof provider,.version=KLJ_NATIVE_IMAGE_PROVIDER_ABI,.owner=owner,.load=load_image,.owns=owns,.symbol=symbol,.native_abi=native_abi,.register_exports=register_exports,.initialize=initialize,.construct=construct,.reject=reject,.invoke=invoke_callback,.jni_context=callback_context};
    char error[512];status=klj_native_image_provider_install(&provider,error,sizeof error);
    if(status){fail(env,error);return status;}
    atomic_store_explicit(&process_images,owner,memory_order_release);return 0;
}
