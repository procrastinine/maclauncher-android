#pragma once
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
// The existing Klepton ABI is the original LP64 Bionic pthread_attr_t layout.
typedef struct {
    uint32_t flags;
    void *stack_base;
    size_t stack_size,guard_size;
    int32_t sched_policy,sched_priority;
    char reserved[16];
} bionic_attr;
typedef struct {int sched_priority;} klj_linux_sched_param;
#if defined(__cplusplus)
static_assert(sizeof(bionic_attr)==56);
extern "C" {
#else
_Static_assert(sizeof(bionic_attr)==56,"LP64 Bionic pthread attributes");
#endif
int klj_pthread_getattr_np(pthread_t,bionic_attr *);
int klb_pthread_attr_init(bionic_attr *);
int klb_pthread_attr_getinheritsched(const bionic_attr *,int *);
#if defined(__cplusplus)
}
#endif
#define BIONIC_ATTR_DETACHED 1
#define BIONIC_ATTR_INHERIT 4
#define BIONIC_ATTR_EXPLICIT 8
