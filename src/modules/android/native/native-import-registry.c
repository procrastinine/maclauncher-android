#include "native-import-registry.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

typedef struct Resolver {
    KljImportResolver resolve;
    struct Resolver *next;
} Resolver;
static _Atomic(Resolver *) head;
static pthread_mutex_t registration=PTHREAD_MUTEX_INITIALIZER;

int klj_register_import_resolver(KljImportResolver resolve) {
    if(!resolve||resolve==klj_resolve_import){errno=EINVAL;return 0;}
    pthread_mutex_lock(&registration);
    Resolver *first=atomic_load_explicit(&head,memory_order_relaxed);
    for(Resolver *entry=first;entry;entry=entry->next)if(entry->resolve==resolve){pthread_mutex_unlock(&registration);return 1;}
    Resolver *entry=malloc(sizeof *entry);
    if(!entry){pthread_mutex_unlock(&registration);return 0;}
    *entry=(Resolver){resolve,first};
    /* Modules and their resolvers live for the process. Each immutable node is
     * published once; readers never lock and duplicate loading allocates none. */
    atomic_store_explicit(&head,entry,memory_order_release);
    pthread_mutex_unlock(&registration);
    return 1;
}
void *klj_resolve_import(const char *name) {
    if(!name)return NULL;
    for(Resolver *entry=atomic_load_explicit(&head,memory_order_acquire);entry;entry=entry->next){void *result=entry->resolve(name);if(result)return result;}
    return NULL;
}
