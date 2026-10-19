/* Guest mutexes have Bionic storage, so their Darwin objects are owned here.
 * Bucket count controls contention only; every bucket grows without a limit.
 * Entries are published under the bucket lock after complete construction.
 * Native calls pin entries; destroy cannot retire an in-flight or held owner. */
#define MTX_BUCKET_COUNT 1024
typedef struct mtx_entry {
    _Atomic(uintptr_t) key;
    _Atomic(pthread_mutex_t *) m;
    _Atomic(void *) owner;
    _Atomic(void *) locksite;
    _Atomic(int) depth;
    _Atomic(unsigned) users;
    int type;
    struct mtx_entry *next;
} mtx_entry;
typedef struct { pthread_mutex_t lock; mtx_entry *head; } mtx_bucket;
static mtx_bucket g_mtx_buckets[MTX_BUCKET_COUNT];
static pthread_once_t g_mtx_once = PTHREAD_ONCE_INIT;

static unsigned mtx_hash(uintptr_t address) {
    return (unsigned)(((address >> 4) * UINT64_C(0x9E3779B97F4A7C15)) >> 49);
}
static void mtx_registry_init(void) {
    for (unsigned i = 0; i < MTX_BUCKET_COUNT; ++i)
        pthread_mutex_init(&g_mtx_buckets[i].lock, NULL);
}
static mtx_bucket *mtx_bucket_for(void *guest) {
    pthread_once(&g_mtx_once, mtx_registry_init);
    return &g_mtx_buckets[mtx_hash((uintptr_t)guest) & (MTX_BUCKET_COUNT - 1)];
}
/* Bionic uses 0/1/2 for normal/recursive/error-checking mutexes, with the
 * static initializer's type stored in bits 14-15. Darwin's enum differs. */
static int mtx_native_type(int type) {
    return type == 0 ? PTHREAD_MUTEX_NORMAL : type == 1 ? PTHREAD_MUTEX_RECURSIVE : PTHREAD_MUTEX_ERRORCHECK;
}
static int mtx_create_host(pthread_mutex_t *mutex, int type) {
    pthread_mutexattr_t attributes;
    int error = pthread_mutexattr_init(&attributes);
    if (!error) {
        error = pthread_mutexattr_settype(&attributes, mtx_native_type(type));
        if (!error) error = pthread_mutex_init(mutex, &attributes);
        pthread_mutexattr_destroy(&attributes);
    }
    return error;
}
static mtx_entry *mtx_entry_for_type(void *guest, int requested_type) {
    if (!guest) return NULL;
    mtx_bucket *bucket = mtx_bucket_for(guest);
    pthread_mutex_lock(&bucket->lock);
    mtx_entry *entry = bucket->head;
    while (entry && atomic_load(&entry->key) != (uintptr_t)guest) entry = entry->next;
    if (!entry) {
        int type = requested_type >= 0 ? requested_type : (atomic_load((_Atomic(uint32_t) *)guest) >> 14) & 3;
        if (type == 3) type = ((const unsigned char *)guest)[4]; /* Bionic LP64 PI mutex. */
        if (type > 2) { pthread_mutex_unlock(&bucket->lock); errno = EINVAL; return NULL; }
        entry = calloc(1, sizeof *entry);
        pthread_mutex_t *mutex = malloc(sizeof *mutex);
        int error = ENOMEM;
        if (entry && mutex) error = mtx_create_host(mutex, type);
        if (error) {
            free(entry); free(mutex);
            pthread_mutex_unlock(&bucket->lock);
            errno = error;
            return NULL;
        }
        atomic_store(&entry->key, (uintptr_t)guest);
        atomic_store(&entry->m, mutex);
        entry->type = type;
        entry->next = bucket->head;
        bucket->head = entry;
    } else if (requested_type >= 0) {
        if (atomic_load(&entry->users) || atomic_load(&entry->depth)) {
            pthread_mutex_unlock(&bucket->lock); errno = EBUSY; return NULL;
        }
        if (entry->type != requested_type) {
            /* Keep the native address stable across legal reinitialization. */
            int error = pthread_mutex_destroy(atomic_load(&entry->m));
            if (!error) error = mtx_create_host(atomic_load(&entry->m), requested_type);
            if (error) { pthread_mutex_unlock(&bucket->lock); errno = error; return NULL; }
            entry->type = requested_type;
        }
    }
    atomic_fetch_add(&entry->users, 1);
    pthread_mutex_unlock(&bucket->lock);
    return entry;
}
static mtx_entry *mtx_entry_for(void *guest) { return mtx_entry_for_type(guest, -1); }
static void mtx_entry_release(mtx_entry *entry) {
    if (entry) atomic_fetch_sub(&entry->users, 1);
}
static pthread_mutex_t *mtx_host(mtx_entry *entry) { return atomic_load(&entry->m); }
/* The kernel drops and reacquires the native mutex inside a condition wait.
 * Accounting must follow that ownership transition, including cancellation. */
static inline int mtx_wait_suspend(mtx_entry *entry) {
    int depth = atomic_exchange(&entry->depth, 0);
    atomic_store(&entry->owner, NULL);
    return depth;
}
static inline void mtx_wait_resume(mtx_entry *entry, int depth, int acquired, void *site) {
    if (acquired && depth == 0) depth = 1;
    atomic_store(&entry->depth, depth);
    atomic_store(&entry->locksite, depth ? site : NULL);
    atomic_store(&entry->owner, depth ? (void *)pthread_self() : NULL);
}
static int mtx_destroy(void *guest) {
    if (!guest) return EINVAL;
    mtx_bucket *bucket = mtx_bucket_for(guest);
    pthread_mutex_lock(&bucket->lock);
    mtx_entry **link = &bucket->head;
    while (*link && atomic_load(&(*link)->key) != (uintptr_t)guest) link = &(*link)->next;
    mtx_entry *entry = *link;
    int result = 0;
    if (entry) {
        if (atomic_load(&entry->users) || atomic_load(&entry->depth)) result = EBUSY;
        else result = pthread_mutex_destroy(mtx_host(entry));
        if (!result) {
            *link = entry->next;
            free(mtx_host(entry));
            free(entry);
        }
    }
    pthread_mutex_unlock(&bucket->lock);
    return result;
}
