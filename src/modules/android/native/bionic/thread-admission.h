#pragma once
#include <stdint.h>
#include <stddef.h>
/* Private additive prototype. Every call stays on the requesting physical
 * pthread. A token may be armed only by an initialized original Bionic parent;
 * it is consumed by that parent's next original pthread_create clone. */
#define KLJ_BIONIC_ADMISSION_ABI 1
#define KLJ_BIONIC_YIELD_NR UINT64_C(0x4d4c1000)
enum { KLJ_BIONIC_READY=1, KLJ_BIONIC_DONE=2, KLJ_BIONIC_EXITED=3 };
typedef struct { uint64_t function,argument,result; } KljBionicAdmissionCall;
typedef struct {
 uint32_t size,version;
 int64_t (*reserve)(uint64_t *token);
 int64_t (*arm)(uint64_t token);
 int64_t (*disarm)(uint64_t token);
 int64_t (*invoke)(uint64_t token,uint64_t function,uint64_t argument,uint64_t *result);
 int64_t (*retire)(uint64_t token);
 uint64_t (*thread_pointer)(void);
 uint64_t (*context)(void);
 /* Private fixture control: actual too-small sigaltstack rejection AFTER
  * native mask/slot mutation must restore every borrowed host field. */
 int64_t (*reject_partial_entry)(uint64_t token);
} KljBionicAdmissionApi;
/* Optional selected host-module initializer. No ordinary module is changed. */
void maclauncher_bionic_admission_install(const KljBionicAdmissionApi *);
