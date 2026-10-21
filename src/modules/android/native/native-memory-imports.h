#ifndef MACLAUNCHER_NATIVE_MEMORY_IMPORTS_H
#define MACLAUNCHER_NATIVE_MEMORY_IMPORTS_H
// Linux mremap over the existing process VM map. Mach moves the actual memory
// object. Extent provenance follows these same serialized mapping mutations;
// allocation and protection remain with the kernel's actual VM objects.
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include "signal-critical.h"

/* Bionic preserves errno and deliberately does not submit POSIX DONTNEED:
 * Linux MADV_DONTNEED discards private modifications, unlike POSIX advice. */
static int klj_posix_madvise(void *address,size_t length,int advice){
    int saved=errno,error=0;
    if(advice<0||advice>4)error=EINVAL;
    else if(advice!=4){
        uintptr_t start=(uintptr_t)address,mask=vm_page_size-1;
        if((start&mask)||length>UINTPTR_MAX-mask)error=EINVAL;
        else{
            size_t rounded=(length+mask)&~mask;
            if(start>UINTPTR_MAX-rounded)error=ENOMEM;
            else{
                mach_vm_address_t cursor=start,end=start+rounded;
                while(cursor<end){
                    mach_vm_address_t base=cursor;mach_vm_size_t size=0;natural_t depth=0;
                    vm_region_submap_info_data_64_t info;mach_msg_type_number_t fields;kern_return_t status;
                    do{fields=VM_REGION_SUBMAP_INFO_COUNT_64;status=mach_vm_region_recurse(mach_task_self(),&base,&size,&depth,(vm_region_recurse_info_t)&info,&fields);if(status!=KERN_SUCCESS)break;if(info.is_submap)depth++;}while(info.is_submap);
                    if(status!=KERN_SUCCESS||base>cursor||size<=cursor-base||size>UINT64_MAX-base){error=ENOMEM;break;}
                    cursor=base+size;
                }
                if(!error){int (*call)(void *,size_t,int)=kl_upstream_shim_lookup("madvise");error=!call?ENOSYS:call(address,length,advice)?errno:0;}
            }
        }
    }
    errno=saved;return kl_errno_to_linux(error);
}

// Linux and Darwin assign different MS_* values. A zero Linux mode is valid
// (asynchronous writeback); Darwin requires an explicit sync/async selection.
static int klj_msync(void *address,size_t length,int flags) {
    if(((uintptr_t)address&(vm_page_size-1))||(flags&~7)||((flags&5)==5)){errno=EINVAL;return -1;}
    if(length>UINTPTR_MAX-(uintptr_t)address){errno=ENOMEM;return -1;}
    if(!length)return 0;
    int native=(flags&4?MS_SYNC:MS_ASYNC)|(flags&2?MS_INVALIDATE:0);
    return msync(address,length,native);
}
static int klj_mincore(void *address,size_t length,unsigned char *vector) {
    if((uintptr_t)address&(vm_page_size-1)){errno=EINVAL;return -1;}
    if(length>UINTPTR_MAX-(uintptr_t)address){errno=ENOMEM;return -1;}
    if(!length)return 0;
    // Darwin may report an unmapped page as nonresident; Linux distinguishes
    // a hole from a mapped nonresident page with ENOMEM. Query the existing
    // Mach VM map, including inaccessible mappings, without tracking copies.
    mach_vm_address_t cursor=(uintptr_t)address,end=cursor+length;
    while(cursor<end){
        mach_vm_address_t base=cursor;mach_vm_size_t extent=0;mach_port_t object=MACH_PORT_NULL;
        vm_region_basic_info_data_64_t info;mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;
        kern_return_t status=mach_vm_region(mach_task_self(),&base,&extent,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&count,&object);
        if(object!=MACH_PORT_NULL)mach_port_deallocate(mach_task_self(),object);
        if(status!=KERN_SUCCESS||base>cursor||extent<=cursor-base||extent>UINT64_MAX-base){errno=ENOMEM;return -1;}
        cursor=base+extent;
    }
    int result=mincore(address,length,(char *)vector);
    if(!result){
        size_t count=length/vm_page_size+(length%vm_page_size!=0);
        // Darwin also reports referenced/modified state. Linux exposes only
        // residency; neither Java nor native callers receive those host bits.
        for(size_t i=0;i<count;i++)vector[i]&=1;
    }
    return result;
}
static int klj_remap_error(kern_return_t status) {
    return status==KERN_INVALID_ADDRESS?EFAULT:status==KERN_PROTECTION_FAILURE?EACCES:ENOMEM;
}
static pthread_mutex_t klj_vm_lock=PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static pthread_once_t klj_vm_once=PTHREAD_ONCE_INIT;
static void klj_vm_before_fork(void){pthread_mutex_lock(&klj_vm_lock);}
static void klj_vm_after_fork(void){pthread_mutex_unlock(&klj_vm_lock);}
/* Darwin's recursive owner token belongs to the parent's physical thread.
 * The child has one thread and a quiescent COW extent snapshot. */
static void klj_vm_child_fork(void){pthread_mutex_t fresh=PTHREAD_RECURSIVE_MUTEX_INITIALIZER;memcpy(&klj_vm_lock,&fresh,sizeof fresh);}
static void klj_vm_initialize(void){pthread_atfork(klj_vm_before_fork,klj_vm_after_fork,klj_vm_child_fork);}
static KljSignalCritical klj_vm_enter(void){KljSignalCritical previous;klj_signal_critical_enter(&previous);pthread_once(&klj_vm_once,klj_vm_initialize);pthread_mutex_lock(&klj_vm_lock);return previous;}
static void klj_vm_leave(const KljSignalCritical *previous){pthread_mutex_unlock(&klj_vm_lock);klj_signal_critical_leave(previous);}
typedef struct KljVmExtent {uintptr_t first,last;int owned;struct KljVmExtent *next;} KljVmExtent;
static KljVmExtent *klj_vm_extents,*klj_vm_spares;
static unsigned klj_vm_spare_count;
/* Reserve bookkeeping before touching a live mapping. Splitting one existing
 * interval and inserting its replacement needs at most two spare nodes. */
static int klj_vm_reserve(unsigned count){
    while(klj_vm_spare_count<count){KljVmExtent *node=malloc(sizeof(*node));if(!node){errno=ENOMEM;return 0;}node->next=klj_vm_spares;klj_vm_spares=node;klj_vm_spare_count++;}
    return 1;
}
static KljVmExtent *klj_vm_take(void){KljVmExtent *node=klj_vm_spares;klj_vm_spares=node->next;klj_vm_spare_count--;return node;}
static void klj_vm_retire(KljVmExtent *node){node->next=klj_vm_spares;klj_vm_spares=node;klj_vm_spare_count++;}
static int klj_vm_bounds(void *address,size_t size,uintptr_t *first,uintptr_t *last){
    uintptr_t mask=vm_page_size-1,start=(uintptr_t)address;
    if((start&mask)||size>UINTPTR_MAX-mask)return 0;
    size_t rounded=(size+mask)&~mask;if(start>UINTPTR_MAX-rounded)return 0;
    *first=start;*last=start+rounded;return 1;
}
static void klj_vm_record(uintptr_t first,uintptr_t last,int owned){
    if(first==last)return;KljVmExtent **link=&klj_vm_extents;
    while(*link&&(*link)->last<=first)link=&(*link)->next;
    while(*link&&(*link)->first<last){
        KljVmExtent *node=*link;
        if(node->first<first){
            if(node->last>last){KljVmExtent *right=klj_vm_take();*right=*node;right->first=last;node->last=first;node->next=right;link=&node->next;break;}
            node->last=first;link=&node->next;continue;
        }
        if(node->last>last){node->first=last;break;}
        *link=node->next;klj_vm_retire(node);
    }
    KljVmExtent *node;
    if(owned){node=klj_vm_take();*node=(KljVmExtent){first,last,1,*link};*link=node;}
    for(node=klj_vm_extents;node&&node->next;){KljVmExtent *next=node->next;if(node->last==next->first&&node->owned==next->owned){node->last=next->last;node->next=next->next;klj_vm_retire(next);}else node=next;}
}
/* Original ELF publication and guest mmap claim actual extents. Retirement
 * removes them, so a native allocator's later reuse cannot inherit ownership.
 * A fresh image publication or guest mmap can claim the same address again. */
static int klj_vm_owned(uintptr_t first,uintptr_t last){
    KljVmExtent *node=klj_vm_extents;
    while(first<last){
        while(node&&node->last<=first)node=node->next;
        if(node&&node->first<=first){if(!node->owned)return 0;first=node->last<last?node->last:last;continue;}
        return 0;
    }
    return 1;
}
static void *klj_mremap_alias_locked(mach_vm_address_t source,mach_vm_size_t size,int flags) {
    // Linux old_size==0 duplicates a shareable mapping. The kernel retains
    // the actual object and offset; closing its file or unmapping the source
    // cannot turn the new view into a private copy.
    if(!(flags&1)){errno=EINVAL;return MAP_FAILED;}
    mach_vm_address_t base=source;mach_vm_size_t extent=0;mach_port_t object=MACH_PORT_NULL;
    vm_region_basic_info_data_64_t info={0};mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;
    kern_return_t result=mach_vm_region(mach_task_self(),&base,&extent,VM_REGION_BASIC_INFO_64,
        (vm_region_info_t)&info,&count,&object);
    if(object!=MACH_PORT_NULL)mach_port_deallocate(mach_task_self(),object);
    if(result!=KERN_SUCCESS||base>source||source-base>=extent){errno=EFAULT;return MAP_FAILED;}
    if(info.inheritance!=VM_INHERIT_SHARE){errno=EINVAL;return MAP_FAILED;}
    // Remapping beyond this original view needs the original file extent,
    // which Mach's region query does not expose. Do not alias adjacent maps.
    if(size>extent-(source-base)){errno=ENOTSUP;return MAP_FAILED;}
    if(info.protection&VM_PROT_EXECUTE){errno=EPERM;return MAP_FAILED;}
    mach_vm_address_t destination=0;vm_prot_t current=0,maximum=0;
    result=mach_vm_remap(mach_task_self(),&destination,size,0,VM_FLAGS_ANYWHERE,
        mach_task_self(),source,FALSE,&current,&maximum,VM_INHERIT_SHARE);
    if(result!=KERN_SUCCESS){errno=klj_remap_error(result);return MAP_FAILED;}
    return (void *)(uintptr_t)destination;
}
static void *klj_mremap_locked(void *address,size_t old_size,size_t new_size,int flags) {
    const mach_vm_size_t page=vm_page_size,mask=page-1;
    mach_vm_address_t start=(uintptr_t)address;
    if((start&mask)||!new_size||(flags&~7)||((flags&6)&&!(flags&1))){errno=EINVAL;return MAP_FAILED;}
    // FIXED and DONTUNMAP need Linux replacement/fault semantics. Decline them
    // before reading a variadic argument, leaving every existing mapping intact.
    if(flags&6){errno=ENOTSUP;return MAP_FAILED;}
    if(old_size>UINT64_MAX-mask||new_size>UINT64_MAX-mask){errno=ENOMEM;return MAP_FAILED;}
    mach_vm_size_t old=(old_size+mask)&~mask,wanted=(new_size+mask)&~mask;
    if(start>UINT64_MAX-old||start>UINT64_MAX-wanted){errno=ENOMEM;return MAP_FAILED;}
    if(!old_size)return klj_mremap_alias_locked(start,wanted,flags);
    vm_region_submap_info_data_64_t info={0};kern_return_t result;
    mach_vm_address_t cursor=start;int first=1;
    while(cursor<start+old){
        mach_vm_address_t base=cursor;mach_vm_size_t extent=0;natural_t depth=0;
        vm_region_submap_info_data_64_t part;mach_msg_type_number_t count=VM_REGION_SUBMAP_INFO_COUNT_64;
        result=mach_vm_region_recurse(mach_task_self(),&base,&extent,&depth,(vm_region_recurse_info_t)&part,&count);
        if(result!=KERN_SUCCESS||part.is_submap||base>cursor||extent<=cursor-base||extent>UINT64_MAX-base){errno=EFAULT;return MAP_FAILED;}
        // Expansion is defined here for anonymous private memory only. Keep
        // file/shared mappings, wired regions and executable/JIT policies intact.
        if(part.external_pager||part.share_mode==SM_SHARED||part.share_mode==SM_TRUESHARED||part.share_mode==SM_SHARED_ALIASED){errno=ENOTSUP;return MAP_FAILED;}
        if(part.user_wired_count){errno=EAGAIN;return MAP_FAILED;}
        if(part.protection&VM_PROT_EXECUTE){errno=EPERM;return MAP_FAILED;}
        if(first){info=part;first=0;}
        else if(part.protection!=info.protection||part.inheritance!=info.inheritance){errno=EFAULT;return MAP_FAILED;}
        info.max_protection&=part.max_protection;
        cursor=base+extent;
    }
    if(wanted==old)return address;
    if(wanted<old){result=mach_vm_deallocate(mach_task_self(),start+wanted,old-wanted);if(result==KERN_SUCCESS)return address;errno=klj_remap_error(result);return MAP_FAILED;}
    mach_vm_address_t tail=start+old;
    // VM_FLAGS_FIXED without OVERWRITE fails atomically if a neighbor exists.
    result=mach_vm_allocate(mach_task_self(),&tail,wanted-old,VM_FLAGS_FIXED);
    if(result==KERN_SUCCESS){
        result=mach_vm_protect(mach_task_self(),tail,wanted-old,TRUE,info.max_protection&~VM_PROT_EXECUTE);
        if(result==KERN_SUCCESS)result=mach_vm_protect(mach_task_self(),tail,wanted-old,FALSE,info.protection);
        if(result==KERN_SUCCESS)result=mach_vm_inherit(mach_task_self(),tail,wanted-old,info.inheritance);
        if(result==KERN_SUCCESS)return address;
        mach_vm_deallocate(mach_task_self(),tail,wanted-old);
    }
    if(!(flags&1)){errno=ENOMEM;return MAP_FAILED;}
    mach_vm_address_t destination=0;
    result=mach_vm_allocate(mach_task_self(),&destination,wanted,VM_FLAGS_ANYWHERE);
    if(result!=KERN_SUCCESS){errno=klj_remap_error(result);return MAP_FAILED;}
    vm_prot_t current=0,maximum=0;mach_vm_address_t mapped=destination;
    // OVERWRITE applies only to the destination reservation just allocated.
    // COPY creates a kernel COW mapping, retaining private ownership without a
    // userspace copy or accidental conversion into a shared alias.
    result=mach_vm_remap(mach_task_self(),&mapped,old,0,VM_FLAGS_FIXED|VM_FLAGS_OVERWRITE,mach_task_self(),start,TRUE,&current,&maximum,info.inheritance);
    if(result==KERN_SUCCESS)result=mach_vm_protect(mach_task_self(),destination+old,wanted-old,TRUE,info.max_protection&~VM_PROT_EXECUTE);
    if(result==KERN_SUCCESS)result=mach_vm_protect(mach_task_self(),destination+old,wanted-old,FALSE,info.protection);
    if(result==KERN_SUCCESS)result=mach_vm_inherit(mach_task_self(),destination+old,wanted-old,info.inheritance);
    if(result==KERN_SUCCESS)result=mach_vm_deallocate(mach_task_self(),start,old);
    if(result!=KERN_SUCCESS){mach_vm_deallocate(mach_task_self(),destination,wanted);errno=klj_remap_error(result);return MAP_FAILED;}
    return (void *)(uintptr_t)destination;
}
static void *klj_mremap(void *address,size_t old_size,size_t new_size,int flags,...) {
    KljSignalCritical previous=klj_vm_enter();
    uintptr_t first,last;int valid=klj_vm_bounds(address,old_size?old_size:new_size,&first,&last);
    int owned=valid&&klj_vm_owned(first,last);
    if(!klj_vm_reserve(4)){klj_vm_leave(&previous);return MAP_FAILED;}
    void *result=klj_mremap_locked(address,old_size,new_size,flags);int error=errno;
    if(result!=MAP_FAILED){
        if(old_size&&klj_vm_bounds(address,old_size,&first,&last))klj_vm_record(first,last,0);
        if(klj_vm_bounds(result,new_size,&first,&last))klj_vm_record(first,last,owned);
    }
    klj_vm_leave(&previous);errno=error;return result;
}
#endif
