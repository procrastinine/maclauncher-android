#ifndef MACLAUNCHER_IMAGE_REGISTRY_H
#define MACLAUNCHER_IMAGE_REGISTRY_H
#include <stddef.h>
#include <stdint.h>
typedef struct KlImageRegistry KlImageRegistry;
typedef struct { const char *soname; void *img; } KlImageRegistryEntry;
typedef struct { const KlImageRegistryEntry *entries; size_t count; } KlImageRegistrySnapshot;
#ifndef __cplusplus
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
typedef struct KlImageRegistryTable {
    struct KlImageRegistryTable *previous;
    size_t capacity;
    _Atomic size_t count;
    KlImageRegistryEntry entries[];
} KlImageRegistryTable;
struct KlImageRegistry {
    _Atomic(KlImageRegistryTable *) current;
    void **owners;
    size_t owner_capacity;
};
static KlImageRegistrySnapshot klj_image_registry_snapshot(KlImageRegistry *registry) {
    KlImageRegistryTable *table=atomic_load_explicit(&registry->current,memory_order_acquire);
    return table?(KlImageRegistrySnapshot){table->entries,atomic_load_explicit(&table->count,memory_order_acquire)}:(KlImageRegistrySnapshot){0};
}
static size_t klj_image_owner_hash(void *image,size_t capacity) {
    uintptr_t key=(uintptr_t)image;key^=key>>17;key*=UINT64_C(0xed5ad4bb);key^=key>>11;
    return (size_t)key&(capacity-1);
}
/* Caller serializes writers. Published entries never change. Geometric prior
 * tables remain alive with their mapped images, so unwind readers need no lock. */
static int klj_image_registry_append(KlImageRegistry *registry,const char *name,void *image) {
    KlImageRegistryTable *table=atomic_load_explicit(&registry->current,memory_order_relaxed);
    size_t count=table?atomic_load_explicit(&table->count,memory_order_relaxed):0;
    if(registry->owner_capacity) {
        size_t slot=klj_image_owner_hash(image,registry->owner_capacity);
        while(registry->owners[slot]) {
            if(registry->owners[slot]==image)return 1;
            slot=(slot+1)&(registry->owner_capacity-1);
        }
    }
    if(count>=registry->owner_capacity/2) {
        size_t capacity=registry->owner_capacity?registry->owner_capacity*2:32;
        if(capacity<registry->owner_capacity||capacity>SIZE_MAX/sizeof(void *))return 0;
        void **owners=calloc(capacity,sizeof(void *));if(!owners)return 0;
        for(size_t i=0;i<count;i++) {
            size_t slot=klj_image_owner_hash(table->entries[i].img,capacity);
            while(owners[slot])slot=(slot+1)&(capacity-1);
            owners[slot]=table->entries[i].img;
        }
        free(registry->owners);registry->owners=owners;registry->owner_capacity=capacity;
    }
    char *copy=strdup(name);if(!copy)return 0;
    if(!table||count==table->capacity) {
        size_t capacity=table?table->capacity*2:16;
        if((table&&capacity<table->capacity)||capacity>(SIZE_MAX-sizeof(*table))/sizeof(KlImageRegistryEntry)){free(copy);return 0;}
        KlImageRegistryTable *next=malloc(sizeof(*next)+capacity*sizeof(KlImageRegistryEntry));
        if(!next){free(copy);return 0;}
        next->previous=table;next->capacity=capacity;
        if(count)memcpy(next->entries,table->entries,count*sizeof(KlImageRegistryEntry));
        next->entries[count]=(KlImageRegistryEntry){copy,image};
        atomic_init(&next->count,count+1);
        atomic_store_explicit(&registry->current,next,memory_order_release);
    }else {
        table->entries[count]=(KlImageRegistryEntry){copy,image};
        atomic_store_explicit(&table->count,count+1,memory_order_release);
    }
    size_t slot=klj_image_owner_hash(image,registry->owner_capacity);
    while(registry->owners[slot])slot=(slot+1)&(registry->owner_capacity-1);
    registry->owners[slot]=image;
    return 1;
}
#endif
#endif
