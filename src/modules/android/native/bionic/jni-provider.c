#include "jni-provider.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
typedef struct {uintptr_t start,end;} ExecutableRange;
struct KljJniImage {
 const void *identity;KljJniImageABI abi;void *owner;
 _Atomic int active;size_t count;struct KljJniImage *next;
 ExecutableRange ranges[];
};
static _Atomic(KljJniImage *) images;
static pthread_mutex_t image_mutex=PTHREAD_MUTEX_INITIALIZER;
static int active(const void *raw){return atomic_load_explicit(&((const KljJniImage *)raw)->active,memory_order_acquire)==1;}
static KljJniImage *find(const void *address){
 uintptr_t value=(uintptr_t)address;
 for(KljJniImage *image=atomic_load_explicit(&images,memory_order_acquire);image;image=image->next){
  if(!active(image))continue;
  for(size_t i=0;i<image->count;i++)if(value>=image->ranges[i].start&&value<image->ranges[i].end)return image;
 }
 return NULL;
}
int klj_jni_provider_address(const void *address){return find(address)!=NULL;}
const void *klj_jni_provider_identity(const void *target){KljJniImage *image=find(target);return image?image->identity:NULL;}
void *klj_jni_provider_native_abi(void *target,const char *descriptor,char *error,size_t capacity){
 KljJniImage *image=find(target);
 if(!image){snprintf(error,capacity,"Native target has no linked image owner");return NULL;}
 return image->abi(image->owner,target,descriptor,error,capacity);
}
void klj_jni_image_abort(KljJniImage *image){if(image)atomic_store_explicit(&image->active,-1,memory_order_release);}
KljJniImage *klj_jni_image_publish(JNIEnv *env,const void *identity,uintptr_t bias,const KljJniProgramHeader *headers,size_t count,jclass defining_class,KljJniImageABI abi,void *owner){
 if(!env||!identity||!headers||!count||!defining_class||!abi||(*env)->ExceptionCheck(env))return NULL;
 size_t executable=0;
 for(size_t i=0;i<count;i++){
  const KljJniProgramHeader *p=&headers[i];if(p->type!=1||!(p->flags&1)||!p->memsz)continue;
  if(p->filesz>p->memsz||p->vaddr>UINTPTR_MAX-bias||p->memsz>UINTPTR_MAX-(bias+p->vaddr))return NULL;
  executable++;
 }
 if(!executable||executable>(SIZE_MAX-sizeof(KljJniImage))/sizeof(ExecutableRange)||!klj_registration_prepare_facade(env))return NULL;
 KljJniImage *image=calloc(1,sizeof(*image)+executable*sizeof(ExecutableRange));if(!image)return NULL;
 image->identity=identity;image->abi=abi;image->owner=owner;image->count=executable;
 size_t at=0;for(size_t i=0;i<count;i++){const KljJniProgramHeader *p=&headers[i];if(p->type==1&&(p->flags&1)&&p->memsz)image->ranges[at++]=(ExecutableRange){bias+p->vaddr,bias+p->vaddr+p->memsz};}
 pthread_mutex_lock(&image_mutex);
 for(KljJniImage *other=atomic_load_explicit(&images,memory_order_acquire);other;other=other->next){
  int state=atomic_load_explicit(&other->active,memory_order_acquire);if(state<0)continue;
  if(other->identity==identity){
   int same=state==1&&other->abi==abi&&other->owner==owner&&other->count==image->count&&!memcmp(other->ranges,image->ranges,image->count*sizeof(ExecutableRange));
   pthread_mutex_unlock(&image_mutex);free(image);if(!same)return NULL;
   for(size_t i=0;i<other->count;i++)if(!klj_class_lookup_bind_range(env,other->ranges[i].start,other->ranges[i].end-other->ranges[i].start,defining_class,other,active))return NULL;
   return other;
  }
  for(size_t i=0;i<other->count;i++)for(size_t j=0;j<image->count;j++)if(image->ranges[j].start<other->ranges[i].end&&other->ranges[i].start<image->ranges[j].end){pthread_mutex_unlock(&image_mutex);free(image);return NULL;}
 }
 /* Metadata remains immutable through VM lifetime. Inactive provisional entries
  * are retained because class lookup may already hold their lifetime predicate. */
 image->next=atomic_load_explicit(&images,memory_order_relaxed);atomic_store_explicit(&images,image,memory_order_release);pthread_mutex_unlock(&image_mutex);
 for(size_t i=0;i<image->count;i++)if(!klj_class_lookup_bind_range(env,image->ranges[i].start,image->ranges[i].end-image->ranges[i].start,defining_class,image,active)){klj_jni_image_abort(image);return NULL;}
 atomic_store_explicit(&image->active,1,memory_order_release);return image;
}
