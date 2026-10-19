#include "library-registry.h"
#include <stdio.h>
KlImageRegistry *klj_library_registry_create(void){return calloc(1,sizeof(KlImageRegistry));}
KlImageRegistrySnapshot klj_library_registry_snapshot(KlImageRegistry *registry){return klj_image_registry_snapshot(registry);}
int klj_library_registry_publish(KlImageRegistry *registry,const char *name,void *image){return klj_image_registry_append(registry,name,image);}
int klj_library_registry_publish_group(KlImageRegistry *registry,const KlImageRegistryEntry *entries,size_t length){
    if(!length)return 1;
    KlImageRegistryTable *table=atomic_load_explicit(&registry->current,memory_order_relaxed);
    size_t count=table?atomic_load_explicit(&table->count,memory_order_relaxed):0;
    if(length>SIZE_MAX-count)return 0;
    const size_t total=count+length;
    size_t capacity=table?table->capacity:16;
    while(capacity<total){if(capacity>SIZE_MAX/2)return 0;capacity*=2;}
    if(capacity>(SIZE_MAX-sizeof(*table))/sizeof(KlImageRegistryEntry))return 0;
    KlImageRegistryTable *next=table;
    if(!table||capacity!=table->capacity){
        next=malloc(sizeof(*next)+capacity*sizeof(KlImageRegistryEntry));if(!next)return 0;
        next->previous=table;next->capacity=capacity;
        if(count)memcpy(next->entries,table->entries,count*sizeof(KlImageRegistryEntry));
    }
    size_t owners_capacity=registry->owner_capacity?registry->owner_capacity:32;
    while(total>=owners_capacity/2){if(owners_capacity>SIZE_MAX/2)goto failed;owners_capacity*=2;}
    if(owners_capacity>SIZE_MAX/sizeof(void *))goto failed;
    void **owners=registry->owners;
    if(owners_capacity!=registry->owner_capacity){
        owners=calloc(owners_capacity,sizeof(void *));if(!owners)goto failed;
        for(size_t i=0;i<count;i++){
            size_t slot=klj_image_owner_hash(table->entries[i].img,owners_capacity);
            while(owners[slot])slot=(slot+1)&(owners_capacity-1);
            owners[slot]=table->entries[i].img;
        }
    }
    size_t copied=0;
    for(;copied<length;copied++){
        char *name=strdup(entries[copied].soname);if(!name)break;
        next->entries[count+copied]=(KlImageRegistryEntry){name,entries[copied].img};
    }
    if(copied!=length){
        for(size_t i=0;i<copied;i++)free((void *)next->entries[count+i].soname);
        if(owners!=registry->owners)free(owners);goto failed;
    }
    for(size_t i=0;i<length;i++){
        size_t slot=klj_image_owner_hash(entries[i].img,owners_capacity);
        while(owners[slot])slot=(slot+1)&(owners_capacity-1);
        owners[slot]=entries[i].img;
    }
    if(owners!=registry->owners){free(registry->owners);registry->owners=owners;registry->owner_capacity=owners_capacity;}
    if(next!=table){atomic_init(&next->count,total);atomic_store_explicit(&registry->current,next,memory_order_release);}
    else atomic_store_explicit(&table->count,total,memory_order_release);
    return 1;
failed:
    if(next!=table)free(next);
    return 0;
}
void klj_library_registry_destroy(KlImageRegistry *registry){
    if(!registry)return;
    KlImageRegistryTable *table=atomic_load(&registry->current);
    if(table)for(size_t i=0;i<atomic_load(&table->count);i++)free((void *)table->entries[i].soname);
    while(table){KlImageRegistryTable *previous=table->previous;free(table);table=previous;}
    free(registry->owners);free(registry);
}

/* Shared mapping/publication transaction. Constructors and JNI_OnLoad are
 * deliberately outside this lock, under the existing per-image state. */
void *klj_library_image_load(pthread_mutex_t *lock,KlImageRegistry *registry,
    const KljLibraryImageOperations *ops,const char *path,const char *name,
    int (*prepare)(void *,void *),void *context,char *error,size_t capacity) {
    char *canonical=realpath(path,NULL);
    if(!canonical)canonical=strdup(path);
    if(!canonical){snprintf(error,capacity,"Native image path allocation failed");return NULL;}
    pthread_mutex_lock(lock);
    if(capacity)error[0]=0;
    KlImageRegistrySnapshot snapshot=klj_image_registry_snapshot(registry);
    void *image=NULL;
    for(size_t i=0;i<snapshot.count;i++) {
        void *candidate=snapshot.entries[i].img;
        const char *mapped=ops->path(ops->context,candidate);
        if(mapped&&!strcmp(mapped,canonical)&&(!ops->accessible||ops->accessible(ops->context,candidate))){image=candidate;break;}
    }
    int fresh=image==NULL;
    if(fresh)image=ops->map(ops->context,canonical);
    if(image&&((ops->validate&&!ops->validate(ops->context,image,error,capacity))||
               (prepare&&!prepare(image,context))||
               (fresh&&!ops->publish(ops->context,name,image)))) {
        if(fresh)ops->unmap(ops->context,image);
        image=NULL;
    }
    pthread_mutex_unlock(lock);
    free(canonical);
    return image;
}
