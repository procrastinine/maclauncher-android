/*
 * Copyright (C) 2024 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Darwin backing for the original ApplicationSharedMemory and
 * PropertyInvalidatedCache native APIs. Nonce layout, atomic publication and
 * byte-block/hash protocol follow AOSP android_app_PropertyInvalidatedCache.
 * HotSpot uses ordinary JNI arguments also for @CriticalNative declarations.
 */
#include <jni.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

enum { MAX_NONCE = 128, MAX_BYTE = 8192, MAX_FEATURE = 512 };
typedef struct {
    int32_t max_nonce, max_byte, nonce_offset, byte_offset;
    _Atomic int32_t hash;
    int32_t padding;
    _Atomic int64_t nonces[MAX_NONCE];
    jbyte bytes[MAX_BYTE];
} NonceStore;
typedef struct {
    _Atomic int64_t network_time;
    int32_t features[MAX_FEATURE];
    _Atomic int64_t feature_count;
    NonceStore system_nonce;
} SharedMemory;
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2, "Shared atomics must be lock-free");
_Static_assert(offsetof(NonceStore, nonces) == 24, "Nonce header ABI");
_Static_assert(sizeof(NonceStore) == 24 + MAX_NONCE * 8 + MAX_BYTE, "Nonce storage ABI");

/* A separately opened O_RDONLY descriptor is retained while the region is
 * mapped. dup() alone would preserve write access. The backing file is unlinked
 * immediately; descriptor identity, not a pathname or reused descriptor number,
 * associates subsequent maps and duplicates with their read-only capability.
 */
typedef struct Region {
    dev_t device;
    ino_t inode;
    int read_only;
    size_t maps;
    struct Region *next;
} Region;
typedef struct Mapping {
    void *pointer;
    Region *region;
    struct Mapping *next;
} Mapping;
static pthread_mutex_t region_mutex = PTHREAD_MUTEX_INITIALIZER;
static Region *regions;
static Mapping *mappings;

static void fail(JNIEnv *env, const char *type, const char *message) {
    if ((*env)->ExceptionCheck(env)) return;
    jclass error = (*env)->FindClass(env, type);
    if (error) { (*env)->ThrowNew(env, error, message); (*env)->DeleteLocalRef(env, error); }
}
static void system_fail(JNIEnv *env, const char *operation, int error) {
    char message[256];
    snprintf(message, sizeof(message), "%s: %s", operation, strerror(error));
    fail(env, "java/lang/RuntimeException", message);
}
static Region *find_region(const struct stat *state) {
    for (Region *region = regions; region; region = region->next)
        if (region->device == state->st_dev && region->inode == state->st_ino) return region;
    return NULL;
}
static void retire_region(Region *region) {
    Region **link = &regions;
    while (*link && *link != region) link = &(*link)->next;
    if (*link) *link = region->next;
    if (region->read_only >= 0) close(region->read_only);
    free(region);
}

JNIEXPORT jint JNICALL Java_com_android_internal_os_ApplicationSharedMemory_nativeCreate(JNIEnv *env, jclass type) {
    (void)type;
    const char *directory = getenv("TMPDIR");
    if (!directory || !*directory) directory = "/tmp";
    char *name = NULL;
    if (asprintf(&name, "%s/application-memory-XXXXXX", directory) < 0) {
        fail(env, "java/lang/OutOfMemoryError", "Application shared memory backing path"); return -1;
    }
    int fd = mkstemp(name), read_only = -1, error = errno;
    if (fd >= 0) {
        read_only = open(name, O_RDONLY | O_CLOEXEC); error = errno;
        if (unlink(name) < 0 && read_only >= 0) { error = errno; close(read_only); read_only = -1; }
    }
    free(name);
    struct stat state;
    Region *region = NULL;
    if (fd < 0 || read_only < 0) goto failure;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || ftruncate(fd, sizeof(SharedMemory)) < 0 || fstat(fd, &state) < 0) {
        error = errno; goto failure;
    }
    region = calloc(1, sizeof(*region));
    if (!region) { error = ENOMEM; goto failure; }
    region->device = state.st_dev; region->inode = state.st_ino; region->read_only = read_only;
    pthread_mutex_lock(&region_mutex);
    region->next = regions; regions = region;
    pthread_mutex_unlock(&region_mutex);
    return fd;
failure:
    if (read_only >= 0) close(read_only);
    if (fd >= 0) close(fd);
    system_fail(env, "Create application shared memory", error);
    return -1;
}

JNIEXPORT jlong JNICALL Java_com_android_internal_os_ApplicationSharedMemory_nativeMap(JNIEnv *env, jclass type, jint fd, jboolean mutable) {
    (void)type;
    struct stat state;
    if (fstat(fd, &state) < 0) { int error = errno; close(fd); system_fail(env, "Inspect application shared memory", error); return 0; }
    int error = EINVAL;
    void *pointer = MAP_FAILED;
    Mapping *mapping = NULL;
    Region *allocated = NULL;
    Region *region = NULL;
    if (state.st_size < (off_t)sizeof(SharedMemory)) goto failure;
    pointer = mmap(NULL, sizeof(SharedMemory), PROT_READ | (mutable ? PROT_WRITE : 0), MAP_SHARED, fd, 0);
    if (pointer == MAP_FAILED) { error = errno; goto failure; }
    mapping = calloc(1, sizeof(*mapping)); allocated = calloc(1, sizeof(*allocated));
    if (!mapping || !allocated) { error = ENOMEM; goto failure; }
    allocated->device = state.st_dev; allocated->inode = state.st_ino; allocated->read_only = -1;
    pthread_mutex_lock(&region_mutex);
    region = find_region(&state);
    if (!region) { region = allocated; allocated = NULL; region->next = regions; regions = region; }
    region->maps++;
    mapping->pointer = pointer; mapping->region = region; mapping->next = mappings; mappings = mapping;
    pthread_mutex_unlock(&region_mutex);
    free(allocated);
    return (jlong)(uintptr_t)pointer;
failure:
    free(mapping); free(allocated);
    if (pointer != MAP_FAILED) munmap(pointer, sizeof(SharedMemory));
    pthread_mutex_lock(&region_mutex);
    region = find_region(&state);
    if (region && !region->maps) retire_region(region);
    pthread_mutex_unlock(&region_mutex);
    close(fd); // Original nativeMap owns descriptor cleanup on mapping failure.
    system_fail(env, "Map application shared memory", error);
    return 0;
}

JNIEXPORT void JNICALL Java_com_android_internal_os_ApplicationSharedMemory_nativeInit(JNIEnv *env, jclass type, jlong pointer) {
    (void)env; (void)type;
    SharedMemory *memory = (SharedMemory *)(uintptr_t)pointer;
    memset(memory, 0, sizeof(*memory));
    atomic_init(&memory->network_time, -1);
    atomic_init(&memory->feature_count, 0);
    NonceStore *nonce = &memory->system_nonce;
    nonce->max_nonce = MAX_NONCE; nonce->max_byte = MAX_BYTE;
    nonce->nonce_offset = offsetof(NonceStore, nonces); nonce->byte_offset = offsetof(NonceStore, bytes);
    atomic_init(&nonce->hash, 0);
    for (int i = 0; i < MAX_NONCE; ++i) atomic_init(&nonce->nonces[i], 0);
}

JNIEXPORT void JNICALL Java_com_android_internal_os_ApplicationSharedMemory_nativeUnmap(JNIEnv *env, jclass type, jlong pointer) {
    (void)type;
    pthread_mutex_lock(&region_mutex);
    Mapping **link = &mappings;
    while (*link && (*link)->pointer != (void *)(uintptr_t)pointer) link = &(*link)->next;
    if (!*link) { pthread_mutex_unlock(&region_mutex); system_fail(env, "Unmap application shared memory", EINVAL); return; }
    Mapping *mapping = *link;
    if (munmap(mapping->pointer, sizeof(SharedMemory)) < 0) {
        int error = errno; pthread_mutex_unlock(&region_mutex); system_fail(env, "Unmap application shared memory", error); return;
    }
    *link = mapping->next;
    if (--mapping->region->maps == 0) retire_region(mapping->region);
    pthread_mutex_unlock(&region_mutex);
    free(mapping);
}

JNIEXPORT jint JNICALL Java_com_android_internal_os_ApplicationSharedMemory_nativeDupAsReadOnly(JNIEnv *env, jclass type, jint fd) {
    (void)type;
    int flags = fcntl(fd, F_GETFL), result = -1, error = errno;
    if (flags >= 0 && (flags & O_ACCMODE) == O_RDONLY) {
        result = fcntl(fd, F_DUPFD_CLOEXEC, 0); error = errno;
    } else if (flags >= 0) {
        struct stat state;
        if (fstat(fd, &state) < 0) error = errno;
        else {
            pthread_mutex_lock(&region_mutex);
            Region *region = find_region(&state);
            if (region && region->read_only >= 0) { result = fcntl(region->read_only, F_DUPFD_CLOEXEC, 0); error = errno; }
            else error = ENOTSUP;
            pthread_mutex_unlock(&region_mutex);
        }
    }
    if (result < 0) system_fail(env, "Duplicate read-only application shared memory", error);
    return result;
}

JNIEXPORT void JNICALL Java_com_android_internal_os_ApplicationSharedMemory_nativeSetLatestNetworkTimeUnixEpochMillisAtZeroElapsedRealtimeMillis(JNIEnv *env, jclass type, jlong pointer, jlong value) {
    (void)env; (void)type;
    atomic_store(&((SharedMemory *)(uintptr_t)pointer)->network_time, value);
}
JNIEXPORT jlong JNICALL Java_com_android_internal_os_ApplicationSharedMemory_nativeGetLatestNetworkTimeUnixEpochMillisAtZeroElapsedRealtimeMillis(JNIEnv *env, jclass type, jlong pointer) {
    (void)env; (void)type;
    return atomic_load(&((SharedMemory *)(uintptr_t)pointer)->network_time);
}
JNIEXPORT jlong JNICALL Java_com_android_internal_os_ApplicationSharedMemory_nativeGetSystemNonceBlock(JNIEnv *env, jclass type, jlong pointer) {
    (void)env; (void)type;
    return (jlong)(uintptr_t)&((SharedMemory *)(uintptr_t)pointer)->system_nonce;
}
JNIEXPORT void JNICALL Java_com_android_internal_os_ApplicationSharedMemory_nativeWriteSystemFeaturesCache(JNIEnv *env, jclass type, jlong pointer, jintArray values) {
    (void)type;
    SharedMemory *memory = (SharedMemory *)(uintptr_t)pointer;
    if (atomic_load(&memory->feature_count) > 0) { fail(env, "java/lang/IllegalStateException", "SystemFeaturesCache already written."); return; }
    if (!values) { fail(env, "java/lang/NullPointerException", "System features"); return; }
    jsize count = (*env)->GetArrayLength(env, values);
    if (count > MAX_FEATURE) { fail(env, "java/lang/IllegalArgumentException", "SystemFeaturesCache exceeds 512 elements"); return; }
    (*env)->GetIntArrayRegion(env, values, 0, count, memory->features);
    if (!(*env)->ExceptionCheck(env)) atomic_store(&memory->feature_count, count);
}
JNIEXPORT jintArray JNICALL Java_com_android_internal_os_ApplicationSharedMemory_nativeReadSystemFeaturesCache(JNIEnv *env, jclass type, jlong pointer) {
    (void)type;
    SharedMemory *memory = (SharedMemory *)(uintptr_t)pointer;
    jsize count = (jsize)atomic_load(&memory->feature_count);
    jintArray result = (*env)->NewIntArray(env, count);
    if (result) (*env)->SetIntArrayRegion(env, result, 0, count, memory->features);
    return result;
}

JNIEXPORT jint JNICALL Java_android_app_PropertyInvalidatedCache_nativeGetMaxNonce(JNIEnv *env, jclass type, jlong pointer) {
    (void)env; (void)type; return ((NonceStore *)(uintptr_t)pointer)->max_nonce;
}
JNIEXPORT jint JNICALL Java_android_app_PropertyInvalidatedCache_nativeGetMaxByte(JNIEnv *env, jclass type, jlong pointer) {
    (void)env; (void)type; return ((NonceStore *)(uintptr_t)pointer)->max_byte;
}
JNIEXPORT jint JNICALL Java_android_app_PropertyInvalidatedCache_nativeGetByteBlockHash(JNIEnv *env, jclass type, jlong pointer) {
    (void)env; (void)type; return atomic_load(&((NonceStore *)(uintptr_t)pointer)->hash);
}
JNIEXPORT void JNICALL Java_android_app_PropertyInvalidatedCache_nativeSetByteBlock(JNIEnv *env, jclass type, jlong pointer, jint hash, jbyteArray values) {
    (void)type;
    if (!values) { fail(env, "java/lang/IllegalArgumentException", "null byte block"); return; }
    NonceStore *nonce = (NonceStore *)(uintptr_t)pointer;
    jsize count = (*env)->GetArrayLength(env, values); if (count > nonce->max_byte) count = nonce->max_byte;
    (*env)->GetByteArrayRegion(env, values, 0, count, (jbyte *)nonce + nonce->byte_offset);
    if (!(*env)->ExceptionCheck(env)) atomic_store(&nonce->hash, hash);
}
JNIEXPORT jint JNICALL Java_android_app_PropertyInvalidatedCache_nativeGetByteBlock(JNIEnv *env, jclass type, jlong pointer, jint hash, jbyteArray values) {
    (void)type;
    NonceStore *nonce = (NonceStore *)(uintptr_t)pointer;
    if (atomic_load(&nonce->hash) == hash) return hash;
    if (!values) { fail(env, "java/lang/NullPointerException", "Byte block"); return 0; }
    jsize count = (*env)->GetArrayLength(env, values); if (count > nonce->max_byte) count = nonce->max_byte;
    (*env)->SetByteArrayRegion(env, values, 0, count, (jbyte *)nonce + nonce->byte_offset);
    return atomic_load(&nonce->hash);
}
JNIEXPORT jlong JNICALL Java_android_app_PropertyInvalidatedCache_nativeGetNonce(JNIEnv *env, jclass type, jlong pointer, jint index) {
    (void)env; (void)type;
    NonceStore *nonce = (NonceStore *)(uintptr_t)pointer;
    if (index < 0 || index >= nonce->max_nonce) return 0;
    return atomic_load(&((_Atomic int64_t *)((char *)nonce + nonce->nonce_offset))[index]);
}
JNIEXPORT jboolean JNICALL Java_android_app_PropertyInvalidatedCache_nativeSetNonce(JNIEnv *env, jclass type, jlong pointer, jint index, jlong value) {
    (void)env; (void)type;
    NonceStore *nonce = (NonceStore *)(uintptr_t)pointer;
    if (index < 0 || index >= nonce->max_nonce) return JNI_FALSE;
    atomic_store(&((_Atomic int64_t *)((char *)nonce + nonce->nonce_offset))[index], value);
    return JNI_TRUE;
}
