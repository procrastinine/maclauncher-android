/* Called only with an unpublished writable copy and its final guest address.
 * AIM owns migration; these are the executor's existing instruction emitters. */
#include "../inline-syscall.h"
#include "kl_x18.h"
#include <libkern/OSCacheControl.h>

extern int kl_errno_to_linux(int);
typedef struct KljPublishedPool {
    uintptr_t start,end;
    void *pool;
    void *staging;
    size_t capacity,pages,remaining;
    struct KljPublishedPool *next;
    unsigned char live[];
} KljPublishedPool;
static pthread_mutex_t published_pool_lock=PTHREAD_MUTEX_INITIALIZER;
static KljPublishedPool *published_pools;

static void finish_executable_code(uintptr_t address,void *staging,size_t size,int success){
    (void)size;
    pthread_mutex_lock(&published_pool_lock);
    KljPublishedPool **link=&published_pools;
    while(*link){
        KljPublishedPool *owner=*link;
        if(owner->start==address&&owner->staging==staging){
            if(success)owner->staging=NULL;
            else{*link=owner->next;munmap(owner->pool,owner->capacity);free(owner);}
            break;
        }
        link=&owner->next;
    }
    pthread_mutex_unlock(&published_pool_lock);
}

void klj_executable_code_forget(void *address,size_t length){
    uintptr_t start=(uintptr_t)address,end=start+length;
    if(end<start)return;
    size_t page=(size_t)getpagesize();
    pthread_mutex_lock(&published_pool_lock);
    KljPublishedPool **link=&published_pools;
    while(*link){
        KljPublishedPool *owner=*link;
        uintptr_t lo=start>owner->start?start:owner->start,hi=end<owner->end?end:owner->end;
        if(lo<hi)for(size_t i=(lo-owner->start)/page;i<(hi-owner->start+page-1)/page;i++)
            if(owner->live[i]){owner->live[i]=0;owner->remaining--;}
        if(!owner->remaining){*link=owner->next;munmap(owner->pool,owner->capacity);free(owner);}
        else link=&owner->next;
    }
    pthread_mutex_unlock(&published_pool_lock);
}

static int64_t prepare_executable_code(uintptr_t address,void *staging,size_t size){
    size_t count=size/4,page=(size_t)getpagesize(),used=0;
    uint8_t *evidence=calloc(count?count:1,1);
    klx_instruction_range *ranges=calloc(count?count:1,sizeof *ranges);
    if(!evidence||!ranges){free(evidence);free(ranges);return -12;}
    uint64_t root=address;
    if(klx_instruction_evidence(staging,count,address,&root,1,evidence)){
        free(evidence);free(ranges);return -12;
    }
    size_t nranges=0;
    for(size_t i=0;i<count;){
        if(!(evidence[i]&1)){i++;continue;}
        size_t begin=i++;while(i<count&&(evidence[i]&1))i++;
        ranges[nranges++]=(klx_instruction_range){address+begin*4,address+i*4};
    }
    klx_code_context context={.address=address,.reachable=ranges,.reachable_count=nranges};
    size_t syscalls=klj_inline_syscall_count_context(staging,size,&context);
    size_t registers=kl_x18_count_context(staging,size,&context);
    if(!syscalls&&!registers){free(evidence);free(ranges);return 0;}
    if(syscalls>(SIZE_MAX-page)/32||registers>(SIZE_MAX-page)/64){free(evidence);free(ranges);return -12;}
    size_t syscall_bytes=(syscalls*32+page-1)&~(page-1);
    size_t register_bytes=(registers*64+page-1)&~(page-1);
    if(syscall_bytes>SIZE_MAX-register_bytes){free(evidence);free(ranges);return -12;}
    size_t capacity=syscall_bytes+register_bytes,pages=(size+page-1)/page;
    KljPublishedPool *owner=calloc(1,sizeof *owner+pages);
    void *pool=owner?kl_x18_reserve_near((void *)address,size,capacity):NULL;
    if(!pool){free(owner);free(evidence);free(ranges);return -(int64_t)kl_errno_to_linux(errno?errno:ENOMEM);}
    int result=klj_inline_syscall_emit_context(staging,size,address,pool,capacity,&used,&context);
    kl_x18_stats stats={0};size_t emitted=0;
    if(!result&&registers){
        result=kl_x18_emit_context(staging,size,address,(char *)pool+used,capacity-used,
            (uintptr_t)pool+used,&stats,&emitted,&context);
        if(!result&&(stats.refused||stats.ctr_refused)){errno=ENOEXEC;result=-1;}
    }
    if(!result)result=mprotect(pool,capacity,PROT_READ|PROT_EXEC);
    int error=errno;free(evidence);free(ranges);
    if(result){munmap(pool,capacity);free(owner);return -(int64_t)kl_errno_to_linux(error?error:ENOEXEC);}
    sys_icache_invalidate(pool,capacity);
    owner->start=address;owner->end=address+size;owner->pool=pool;owner->staging=staging;owner->capacity=capacity;
    owner->pages=owner->remaining=pages;memset(owner->live,1,pages);
    pthread_mutex_lock(&published_pool_lock);owner->next=published_pools;published_pools=owner;pthread_mutex_unlock(&published_pool_lock);
    return 0;
}
