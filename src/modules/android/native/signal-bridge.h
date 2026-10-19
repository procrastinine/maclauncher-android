#pragma once
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include "signal-critical.h"
#ifdef __cplusplus
extern "C" {
#endif
int klj_signal_to_host(int signal);
int klj_signal_to_guest(int signal);
sigset_t klj_signal_mask_to_host(uint64_t mask);
uint64_t klj_signal_mask_to_guest(sigset_t mask);
typedef struct {int32_t signal,error,code,padding;union {
 struct {int32_t pid;uint32_t uid;union {int32_t status;uint64_t value;};int64_t user_time,system_time;} sender;
 struct {void *address;uint64_t detail[4];} fault;
 struct {int64_t band;int32_t fd;} poll;
 uint64_t storage[14];
} data;} klj_signal_info;
void klj_signal_info_to_guest(const siginfo_t *native,klj_signal_info *guest);
int klj_signal_is_hardware_fault(int signal,const siginfo_t *info);
int klj_signal_mask(int how,const uint64_t *mask,uint64_t *previous);
int klj_signal_thread(pthread_t thread,int signal);
int klj_signal_thread_id(int process,int thread,int signal);
void *klj_signal_lookup(const char *name);
#ifdef __cplusplus
}
#endif
