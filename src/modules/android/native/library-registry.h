#pragma once
#include "image-registry.h"
#include <pthread.h>
#ifdef __cplusplus
extern "C" {
#endif
/* The existing append-only image table remains the publication/unwind owner.
 * Each VM supplies only its image representation and native binding checks. */
typedef struct {
    void *context;
    const char *(*path)(void *,void *image);
    int (*accessible)(void *,void *image);
    void *(*map)(void *,const char *path);
    void (*unmap)(void *,void *image);
    int (*validate)(void *,void *image,char *error,size_t capacity);
    int (*publish)(void *,const char *name,void *image);
} KljLibraryImageOperations;
KlImageRegistry *klj_library_registry_create(void);
/* Destroy only after image execution and all snapshot readers have retired. */
void klj_library_registry_destroy(KlImageRegistry *);
KlImageRegistrySnapshot klj_library_registry_snapshot(KlImageRegistry *);
int klj_library_registry_publish(KlImageRegistry *,const char *name,void *image);
/* Entries are new owners, distinct from each other and the published table. */
int klj_library_registry_publish_group(KlImageRegistry *,const KlImageRegistryEntry *,size_t);
void *klj_library_image_load(pthread_mutex_t *,KlImageRegistry *,
    const KljLibraryImageOperations *,const char *path,const char *name,
    int (*prepare)(void *image,void *),void *prepare_context,char *error,size_t capacity);
/* Group loading follows the original linker sequence: discover/map all
 * dependencies, relocate against the complete group, then publish. The
 * existing image representation retains dependency edges and constructor
 * state. A resolved native dependency remains with its platform linker. */
typedef struct {
    void *context;
    const char *(*path)(void *,void *image);
    int (*contains)(void *,void *space,void *image);
    void *(*map)(void *,void *space,const char *path,char *,size_t);
    void (*unmap)(void *,void *image);
    int (*dependencies)(void *,const char *,int (*)(void *,const char *),void *,char *,size_t);
    /* 1: translated image, 2: retained platform-linker dependency, 0: error. */
    int (*resolve)(void *,void *space,void *requester_image,const char *requester,const char *name,
                   void **provider,char *path,size_t capacity,char *,size_t);
    void *(*symbol)(void *,void *image,const char *);
    int (*global)(void *,void *space,void *image);
    int (*relocate)(void *,void *image,void *(*)(void *,const char *),void *,char *,size_t);
    int (*dependencies_set)(void *,void *image,void *const *,size_t);
    int (*validate)(void *,void *image,char *,size_t);
    /* Commit must publish the whole group or leave the registry unchanged. */
    int (*publish)(void *,const KlImageRegistryEntry *,void *const *spaces,size_t);
    void *(*external_symbol)(void *,void *image,const char *name);
    void (*children)(void *,void *image,void (*visit)(void *,void *),void *);
    void (*group_root)(void *,void *image,void *root);
} KljLibraryGroupOperations;
void *klj_library_group_load(pthread_mutex_t *,KlImageRegistry *,
    const KljLibraryGroupOperations *,void *space,const char *path,const char *name,
    int (*prepare)(void *,void *),void *,char *error,size_t capacity);
/* Original Bionic dlsym handle scope: accessible dependency closure in breadth
 * first order, optionally beginning after an existing caller (RTLD_NEXT). */
void *klj_library_group_symbol(const KljLibraryGroupOperations *,void *space,
    void *root,void *skip_until,const char *name);
#ifdef __cplusplus
}
#endif
