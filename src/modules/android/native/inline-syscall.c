/* Linux SVC and TP instructions share the original owners and image arena. */
#include "inline-syscall.h"
#include "kl_x18.h"
#include "executable-memory/api.h"
#include "executable-memory/jvm.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>

extern void klj_inline_syscall_enter(void);
extern long klb_syscall(long,long,long,long,long,long,long);
extern int kl_errno_to_linux(int);

static int thread_pointer_instruction(uint32_t word){
    return ((word&0xffffffe0u)==0xd53bd040u&&(word&31)!=31)||(word&0xffffffe0u)==0xd51bd040u;
}
int klj_inline_instruction_match(uint32_t word){return word==0xd4000001u||thread_pointer_instruction(word);}

long klj_inline_syscall_dispatch(const uint64_t *registers){
    int saved_errno=errno;
    long result=klb_syscall((long)registers[8],(long)registers[0],(long)registers[1],
        (long)registers[2],(long)registers[3],(long)registers[4],(long)registers[5]);
    // SVC returns a negative Linux errno, whereas libc syscall returns -1 and
    // updates errno. Do not leak that libc implementation detail to the guest.
    if(result==-1)result=-kl_errno_to_linux(errno);
    errno=saved_errno;return result;
}
size_t klj_inline_syscall_count_context(const void *code,size_t size,const klx_code_context *context){
    const uint32_t *words=code;size_t count=0;
    for(size_t i=0;i<size/4;i++)if(klj_inline_instruction_match(words[i])&&!kl_x18_is_data_context(code,size,i,context))count++;
    return count;
}
static int branch(uintptr_t from,uintptr_t to,uint32_t *instruction){
    int64_t distance=(int64_t)to-(int64_t)from;
    if((distance&3)||distance<-(1LL<<27)||distance>=(1LL<<27))return -1;
    *instruction=0x14000000u|((uint32_t)(distance/4)&0x03ffffffu);return 0;
}
static int instruction_body(uint32_t word,uint32_t output[8]){
    for(size_t i=0;i<8;i++)output[i]=0xd503201fu;
    if(thread_pointer_instruction(word)){
        int offset=klj_guest_thread_pointer_offset();if(offset<0)return -1;
        int64_t size=klj_guest_thread_pointer_stub(word,(uint32_t)offset,KLX_TSD_SLOT*8,output,7);
        if(size<0){errno=EINVAL;return -1;}
        return (int)size;
    }
    output[0]=0xa9bf7bf0u; // stp x16,x30,[sp,#-16]!
    output[1]=0x580000b0u; // ldr x16,literal at +24
    output[2]=0xd63f0200u; // blr x16
    output[3]=0xa8c17bf0u; // ldp x16,x30,[sp],#16
    uintptr_t target=(uintptr_t)klj_inline_syscall_enter;memcpy(output+6,&target,sizeof target);
    return 4;
}
int klj_inline_syscall_patch_context(void *code,size_t size,void *arena,size_t arena_size,size_t *arena_used,const klx_code_context *context){
    return klj_inline_syscall_emit_context(code,size,(uintptr_t)code,arena,arena_size,arena_used,context);
}
int klj_inline_syscall_emit_context(void *code,size_t size,uintptr_t address,void *arena,size_t arena_size,size_t *arena_used,const klx_code_context *context){
    if(!klj_inline_syscall_count_context(code,size,context))return 0;
    if(!arena_used){errno=ENOMEM;return -1;}
    KljInlinePool pool={arena,arena_size,*arena_used,0};
    extern int klj_inline_syscall_emit_pools_context(void *,size_t,uintptr_t,KljInlinePool *,size_t,const klx_code_context *);
    int result=klj_inline_syscall_emit_pools_context(code,size,address,&pool,1,context);
    *arena_used=pool.used;return result;
}
static size_t pool_capacity(size_t bytes,const KljInlinePool *pool,size_t page){return pool->packed?bytes:(bytes+page-1)&~(page-1);}
static int pool_protect(void *base,size_t bytes,int protection){
    if(!bytes)return 0;
    uintptr_t page=(uintptr_t)getpagesize(),first=(uintptr_t)base&~(page-1),end=((uintptr_t)base+bytes+page-1)&~(page-1);
    return mprotect((void *)first,end-first,protection);
}
int klj_inline_syscall_pools_seal(KljInlinePool *pools,size_t count){
    for(size_t n=0;n<count;n++)if(pools[n].used&&pool_protect(pools[n].base,pools[n].used,PROT_READ|PROT_EXEC))return -1;
    return 0;
}
int klj_inline_instruction_body(uint32_t word,uintptr_t site,uintptr_t target,uint32_t *body,size_t *bytes,uint32_t *entry){
    int back=instruction_body(word,body);if(back<0)return -1;
    if(branch(site,target,entry)||branch(target+(size_t)back*4,site+4,&body[back])){errno=ERANGE;return -1;}
    *bytes=32;return 0;
}
int klj_inline_plan_apply(void *code,uintptr_t address,KljInlineSite *sites,size_t count,KljInlinePool *pools,size_t pool_count,KljInlineEmitter emit){
    return klj_inline_plan_apply_aliases(code,address,sites,count,pools,pool_count,emit,NULL);
}
int klj_inline_plan_apply_aliases(void *code,uintptr_t address,KljInlineSite *sites,size_t count,KljInlinePool *pools,size_t pool_count,KljInlineEmitter emit,void *const *aliases){
    if(!count)return 0;
    size_t page=(size_t)getpagesize();
    if(!pools||!pool_count||pool_count>SIZE_MAX/sizeof(size_t)){errno=ENOMEM;return -1;}
    size_t *needed=calloc(pool_count,sizeof *needed);uint8_t *attempted=calloc(pool_count,1);
    if(!needed||!attempted){free(needed);free(attempted);return -1;}
    int error=0;uint32_t *words=code;
    // Original owners preclassify every site. Preflight each complete original
    // body at its future address before changing any guest word or pool usage.
    for(size_t n=0;n<count;n++){
        KljInlineSite *site=&sites[n];uintptr_t pc=address+site->word*4;
        int space=0,body_error=0;size_t selected=SIZE_MAX;
        memset(attempted,0,pool_count);
        for(size_t attempt=0;attempt<pool_count;attempt++){
            size_t candidate=SIZE_MAX;uint64_t best=UINT64_MAX;
            for(size_t p=0;p<pool_count;p++)if(!attempted[p]){
                KljInlinePool *pool=&pools[p];
                if(!pool->base||pool->used>pool->size||needed[p]>SIZE_MAX-sizeof site->body-page)continue;
                size_t capacity=pool_capacity(needed[p]+sizeof site->body,pool,page);
                if(capacity>pool->size-pool->used)continue;
                space=1;uintptr_t target=(uintptr_t)pool->base+pool->used+needed[p];uint32_t entry;
                if(branch(pc,target,&entry))continue;
                uint64_t distance=target>pc?target-pc:pc-target;
                // Keep small interior gaps for sites beyond both outer arenas.
                if(candidate!=SIZE_MAX&&pools[candidate].packed<pool->packed)continue;
                if(candidate!=SIZE_MAX&&pools[candidate].packed==pool->packed&&distance>=best)continue;
                candidate=p;best=distance;
            }
            if(candidate==SIZE_MAX)break;
            attempted[candidate]=1;KljInlinePool *pool=&pools[candidate];
            uintptr_t target=(uintptr_t)pool->base+pool->used+needed[candidate];
            uint32_t body[12],entry;size_t bytes=0;
            if(emit(words[site->word],pc,target,body,&bytes,&entry)){body_error=errno;continue;}
            if(!bytes||bytes>sizeof body||(bytes&3)){body_error=EINVAL;continue;}
            selected=candidate;site->entry=entry;site->bytes=bytes;memcpy(site->body,body,bytes);break;
        }
        if(selected==SIZE_MAX){error=body_error?body_error:space?ERANGE:ENOMEM;break;}
        site->pool=selected;site->offset=needed[selected];needed[selected]+=site->bytes;
    }
    if(!error)for(size_t p=0;p<pool_count;p++)if(needed[p]&&pool_protect((char *)(aliases&&aliases[p]?aliases[p]:pools[p].base)+pools[p].used,pool_capacity(needed[p],&pools[p],page),PROT_READ|PROT_WRITE)){error=errno;break;}
    if(!error){
        for(size_t n=0;n<count;n++){
            KljInlineSite *site=&sites[n];KljInlinePool *pool=&pools[site->pool];
            memcpy((char *)(aliases&&aliases[site->pool]?aliases[site->pool]:pool->base)+pool->used+site->offset,site->body,site->bytes);words[site->word]=site->entry;
        }
        for(size_t p=0;p<pool_count;p++)if(needed[p]){
            size_t capacity=pool_capacity(needed[p],&pools[p],page);
            sys_icache_invalidate((char *)(aliases&&aliases[p]?aliases[p]:pools[p].base)+pools[p].used,capacity);pools[p].used+=capacity;
        }
        for(size_t p=0;p<pool_count;p++)if(needed[p]&&pool_protect(aliases&&aliases[p]?aliases[p]:pools[p].base,pools[p].used,PROT_READ|PROT_EXEC)){error=errno;break;}
    }
    free(attempted);free(needed);if(error){errno=error;return -1;}return 0;
}
int klj_inline_syscall_emit_pools_context(void *code,size_t size,uintptr_t address,KljInlinePool *pools,size_t pool_count,const klx_code_context *context){
    size_t count=klj_inline_syscall_count_context(code,size,context);if(!count)return 0;
    if(count>SIZE_MAX/sizeof(KljInlineSite)){errno=ENOMEM;return -1;}
    KljInlineSite *sites=calloc(count,sizeof *sites);if(!sites)return -1;
    uint32_t *words=code;size_t next=0;
    for(size_t i=0;i<size/4;i++)if(klj_inline_instruction_match(words[i])&&!kl_x18_is_data_context(code,size,i,context))sites[next++].word=i;
    int result=klj_inline_plan_apply(code,address,sites,count,pools,pool_count,klj_inline_instruction_body);free(sites);return result;
}
int klj_inline_syscall_patch_pools_context(void *code,size_t size,KljInlinePool *pools,size_t count,const klx_code_context *context){return klj_inline_syscall_emit_pools_context(code,size,(uintptr_t)code,pools,count,context);}
size_t klj_inline_syscall_count(const void *code,size_t size){return klj_inline_syscall_count_context(code,size,NULL);}
int klj_inline_syscall_patch(void *code,size_t size,void *arena,size_t arena_size,size_t *used){return klj_inline_syscall_patch_context(code,size,arena,arena_size,used,NULL);}
