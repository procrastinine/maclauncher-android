#pragma once
#include <mach/mach.h>
#include <mach/mach_vm.h>

static pthread_once_t klj_vm_trace_once=PTHREAD_ONCE_INIT;
static int klj_vm_trace_enabled;
static void klj_vm_trace_configure(void) { klj_vm_trace_enabled=kl_env_on("KL_TRACE_VM",0); }
/* Observe executable mappings and guest protection changes without changing
 * their permissions or result. Data-only mprotect can be decisive when a guest
 * edits its own ELF metadata before restoring the original mapping flags.
 * The live Mach mapping and existing ELF owner supply provenance; no parallel
 * address registry is maintained by diagnostics. */
static void klj_vm_trace(const char *operation,void *address,size_t length,
                         int protection,int result,int error,const void *caller) {
    pthread_once(&klj_vm_trace_once,klj_vm_trace_configure);
    if(!klj_vm_trace_enabled)return;
    if(!(protection&PROT_EXEC)&&strcmp(operation,"mprotect"))return;
    static unsigned records;
    unsigned record=__atomic_fetch_add(&records,1,__ATOMIC_RELAXED);
    if(record>=256){if(record==256)fputs("[android-vm] further mapping trace records suppressed\n",stderr);return;}
    mach_vm_address_t base=(uintptr_t)address;mach_vm_size_t size=0;
    natural_t depth=0;vm_region_submap_info_data_64_t info={0};
    mach_msg_type_number_t count=VM_REGION_SUBMAP_INFO_COUNT_64;
    kern_return_t status=mach_vm_region_recurse(mach_task_self(),&base,&size,&depth,
        (vm_region_recurse_info_t)&info,&count);
    size_t owner_offset=0,caller_offset=0;
    const char *owner=kl_addr_image(address,&owner_offset);
    const char *from=kl_addr_image(caller,&caller_offset);
    fprintf(stderr,"[android-vm] %s address=%p length=%#zx requested=%d result=%d errno=%d "
        "region=%#llx+%#llx current=%d maximum=%d query=%d owner=%s+%#zx caller=%s+%#zx\n",
        operation,address,length,protection,result,error,(unsigned long long)base,
        (unsigned long long)size,info.protection,info.max_protection,status,
        owner?owner:"anonymous",owner_offset,from?from:"host",caller_offset);
}
