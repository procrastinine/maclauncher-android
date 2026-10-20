/* AIM publishes one private writable snapshot at its final guest address.
 * Original instruction owners share a single reachable-pool transaction. */
#include "../inline-syscall.h"
#include "../x18-pools.h"
#include "../image-pool-lease.h"
#include <libkern/OSCacheControl.h>

extern int kl_errno_to_linux(int);
typedef struct {
    KljImagePoolLease image;
    size_t skip,used;
    int committed,released;
} KljPublishedLease;
typedef struct KljPublishedPool {
    uintptr_t start,end;
    void *staging;
    void *outer[2];
    size_t capacity,pages,remaining,lease_count;
    KljPublishedLease *leases;
    struct KljPublishedPool *next;
    unsigned char live[];
} KljPublishedPool;
static pthread_mutex_t published_pool_lock=PTHREAD_MUTEX_INITIALIZER;
static KljPublishedPool *published_pools;

static void destroy_published_pool(KljPublishedPool *owner){
    for(size_t i=0;i<2;i++)if(owner->outer[i])munmap(owner->outer[i],owner->capacity);
    free(owner->leases);free(owner);
}
/* A retired older reservation may precede another live publication. Retain its
 * tiny owner until the later tail retires; never recycle an executing body. */
static void collect_published_pools(void){
    int changed;
    do {
        changed=0;KljPublishedPool **link=&published_pools;
        while(*link){
            KljPublishedPool *owner=*link;
            if(owner->remaining||owner->staging){link=&owner->next;continue;}
            int retained=0;
            for(size_t i=owner->lease_count;i;i--){KljPublishedLease *lease=&owner->leases[i-1];
                if(lease->released||!lease->used)continue;
                if(!lease->committed||klj_image_gap_pool_release(&lease->image,lease->skip+lease->used))lease->released=1;
                else retained=1;
            }
            if(retained){link=&owner->next;continue;}
            *link=owner->next;destroy_published_pool(owner);changed=1;
        }
    }while(changed);
}
static void finish_executable_code(uintptr_t address,void *staging,size_t size,int success){
    (void)size;pthread_mutex_lock(&published_pool_lock);
    for(KljPublishedPool *owner=published_pools;owner;owner=owner->next)if(owner->start==address&&owner->staging==staging){
        if(success)for(size_t i=0;i<owner->lease_count;i++){KljPublishedLease *lease=&owner->leases[i];
            if(lease->used){klj_image_gap_pool_commit(&lease->image,lease->skip+lease->used);lease->committed=1;}
        }
        owner->staging=NULL;if(!success)owner->remaining=0;break;
    }
    collect_published_pools();pthread_mutex_unlock(&published_pool_lock);
}
void klj_executable_code_forget(void *address,size_t length){
    uintptr_t start=(uintptr_t)address,end=start+length;if(end<start)return;
    size_t page=(size_t)getpagesize();pthread_mutex_lock(&published_pool_lock);
    for(KljPublishedPool *owner=published_pools;owner;owner=owner->next){
        uintptr_t lo=start>owner->start?start:owner->start,hi=end<owner->end?end:owner->end;
        if(lo<hi)for(size_t i=(lo-owner->start)/page;i<(hi-owner->start+page-1)/page;i++)
            if(owner->live[i]){owner->live[i]=0;owner->remaining--;}
    }
    collect_published_pools();pthread_mutex_unlock(&published_pool_lock);
}
static int64_t prepare_executable_code(uintptr_t address,void *staging,size_t size){
    size_t count=size/4,page=(size_t)getpagesize();
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
    if(syscalls>SIZE_MAX-registers||syscalls+registers>(SIZE_MAX-page)/48){free(evidence);free(ranges);return -12;}
    size_t capacity=((syscalls+registers)*48+page-1)&~(page-1),pages=(size+page-1)/page;
    size_t available=klj_image_gap_pool_leases(address,size,NULL,0);
    if(available>SIZE_MAX/sizeof(KljImagePoolLease)||available>SIZE_MAX/sizeof(KljPublishedLease)-2){free(evidence);free(ranges);return -12;}
    KljImagePoolLease *loans=calloc(available?available:1,sizeof *loans);
    KljInlinePool *pools=calloc(available+2,sizeof *pools);
    void **aliases=calloc(available+2,sizeof *aliases);
    KljPublishedPool *owner=calloc(1,sizeof *owner+pages);
    if(!loans||!pools||!aliases||!owner){free(loans);free(pools);free(aliases);free(owner);free(evidence);free(ranges);return -12;}
    owner->leases=calloc(available?available:1,sizeof *owner->leases);
    if(!owner->leases){free(loans);free(pools);free(aliases);free(owner);free(evidence);free(ranges);return -12;}
    owner->capacity=capacity;size_t pool_count=0;
    // Each outer arena need only reach its neighboring sites. The planner
    // checks every complete body and return against all available arenas.
    for(size_t i=0;i<2;i++){
        uintptr_t at=i?address+size-4:address;
        void *pool=kl_x18_reserve_near((void *)at,4,capacity);
        if(pool){owner->outer[i]=pool;pools[pool_count++]=(KljInlinePool){pool,capacity,0,0};}
    }
    size_t returned=klj_image_gap_pool_leases(address,size,loans,available);
    if(returned>available){errno=EAGAIN;goto failed;}
    for(size_t i=0;i<returned;i++){
        KljImagePoolLease loan=loans[i];uintptr_t lo=(uintptr_t)loan.pool.base,hi=lo+loan.pool.size;
        if(hi<lo||hi<=address||lo>=address+size)continue;
        uintptr_t first=lo>address?lo:address,last=hi<address+size?hi:address+size;
        size_t skip=first-lo;
        pools[pool_count]=(KljInlinePool){(void *)first,last-first,0,1};
        aliases[pool_count]=(char *)staging+(first-address);
        owner->leases[owner->lease_count++]=(KljPublishedLease){.image=loan,.skip=skip};
        pool_count++;
    }
    kl_x18_stats stats;
    int result=klj_image_instruction_emit_pools_context(staging,size,address,pools,pool_count,&stats,&context,aliases);
    if(result)goto failed;
    // Outer entries precede borrowed entries, but failed outer allocations do
    // not leave holes in the planner's array.
    size_t outer_count=(owner->outer[0]!=NULL)+(owner->outer[1]!=NULL);
    for(size_t i=0;i<owner->lease_count;i++)owner->leases[i].used=pools[outer_count+i].used;
    owner->start=address;owner->end=address+size;owner->staging=staging;
    owner->pages=owner->remaining=pages;memset(owner->live,1,pages);
    pthread_mutex_lock(&published_pool_lock);owner->next=published_pools;published_pools=owner;pthread_mutex_unlock(&published_pool_lock);
    free(loans);free(pools);free(aliases);free(evidence);free(ranges);return 0;
failed:{
    int error=errno;destroy_published_pool(owner);free(loans);free(pools);free(aliases);free(evidence);free(ranges);
    return -(int64_t)kl_errno_to_linux(error?error:ENOEXEC);
    }
}
