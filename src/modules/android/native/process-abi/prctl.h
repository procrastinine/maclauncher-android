#pragma once
#include "process-abi/prctl-constants.h"
#include "process-ids.h"
#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

// This is the fixed kernel argument ABI used by Bionic's original prctl
// assembly entry. Both guest CPU widths reach this same host policy owner.
static int klj_process_prctl(int operation,uintptr_t first,uintptr_t second,uintptr_t third,uintptr_t fourth){
 (void)second;(void)third;(void)fourth;
 if(operation==PR_SET_NAME){
  char name[16]={0};
  for(unsigned i=0;i<15;i++){
   mach_vm_size_t copied=0;
   if(mach_vm_read_overwrite(mach_task_self(),(mach_vm_address_t)first+i,1,(mach_vm_address_t)&name[i],&copied)!=KERN_SUCCESS||copied!=1){errno=EFAULT;return -1;}
   if(!name[i])break;
  }
  int error=pthread_setname_np(name);if(error){errno=error;return -1;}return 0;
 }
 if(operation==PR_GET_NAME){
  char native[64]={0},name[16]={0};int error=pthread_getname_np(pthread_self(),native,sizeof(native));
  if(error){errno=error;return -1;}memcpy(name,native,15);
  if(mach_vm_write(mach_task_self(),(mach_vm_address_t)first,(vm_offset_t)name,sizeof(name))!=KERN_SUCCESS){errno=EFAULT;return -1;}return 0;
 }
 if(operation==PR_GET_DUMPABLE)return klj_process_get_dumpable();
 if(operation==PR_SET_DUMPABLE){
  if(first>1){errno=EINVAL;return -1;}
  return klj_process_set_dumpable((int)first);
 }
 // Yama tracer exceptions are distinct from dumpability and do not grant
 // debugger authority on the host.
 if(operation==PR_SET_PTRACER){errno=ENOTSUP;return -1;}
 // Other Linux process controls are not recognized by this host kernel.
 errno=EINVAL;return -1;
}
