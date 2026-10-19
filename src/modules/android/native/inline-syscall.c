/* Linux SVC and TP instructions share the original owners and image arena. */
#include "inline-syscall.h"
#include "kl_x18.h"
#include "executable-memory/api.h"
#include "executable-memory/jvm.h"
#include <errno.h>
#include <stdint.h>
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
static int adapted_instruction(uint32_t word){return word==0xd4000001u||thread_pointer_instruction(word);}

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
    for(size_t i=0;i<size/4;i++)if(adapted_instruction(words[i])&&!kl_x18_is_data_context(code,size,i,context))count++;
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
    size_t count=klj_inline_syscall_count_context(code,size,context);if(!count)return 0;
    size_t page=(size_t)getpagesize();
    if(count>(SIZE_MAX-page)/32){errno=ENOMEM;return -1;}
    size_t capacity=(count*32+page-1)&~(page-1);
    if(!arena||!arena_used||*arena_used>arena_size||capacity>arena_size-*arena_used){errno=ENOMEM;return -1;}
    uint32_t *words=code,*pool=(void *)((uint8_t *)arena+*arena_used);size_t next=0;
    // Validate every displacement before changing any guest instruction.
    for(size_t i=0;i<size/4;i++)if(adapted_instruction(words[i])&&!kl_x18_is_data_context(code,size,i,context)){
        uint32_t ignored,body[8];int back=instruction_body(words[i],body);if(back<0)return -1;
        if(branch(address+i*4,(uintptr_t)&pool[next],&ignored)||
           branch((uintptr_t)&pool[next+(size_t)back],address+(i+1)*4,&ignored)){errno=ERANGE;return -1;}
        next+=8;
    }
    if(mprotect(pool,capacity,PROT_READ|PROT_WRITE))return -1;
    next=0;
    for(size_t i=0;i<size/4;i++)if(adapted_instruction(words[i])&&!kl_x18_is_data_context(code,size,i,context)){
        uint32_t *veneer=pool+next,entry;
        int back=instruction_body(words[i],veneer);if(back<0)return -1;
        (void)branch((uintptr_t)&veneer[back],address+(i+1)*4,&veneer[back]);
        (void)branch(address+i*4,(uintptr_t)veneer,&entry);words[i]=entry;next+=8;
    }
    *arena_used+=capacity;
    if(mprotect(pool,capacity,PROT_READ|PROT_EXEC))return -1;
    sys_icache_invalidate(pool,capacity);return 0;
}
size_t klj_inline_syscall_count(const void *code,size_t size){return klj_inline_syscall_count_context(code,size,NULL);}
int klj_inline_syscall_patch(void *code,size_t size,void *arena,size_t arena_size,size_t *used){return klj_inline_syscall_patch_context(code,size,arena,arena_size,used,NULL);}
