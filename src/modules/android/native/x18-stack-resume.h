/* Shared completion for dynamic SP arithmetic and SIMD structure veneers.
 * The original operation executes on the CPU. Only scratch restoration and SP
 * commit use the already interrupted Darwin context, without a physical x18
 * value, allocation, guest callback or released stack storage. */
#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/ucontext.h>

static int klj_x18_copy(uint64_t address,void *output,size_t size) {
    mach_vm_size_t copied=0;
    return mach_vm_read_overwrite(mach_task_self(),address,size,
        (mach_vm_address_t)(uintptr_t)output,&copied)==KERN_SUCCESS && copied==size;
}

static int klj_x18_stack_resume(void *context) {
    if(!context)return 0;
    ucontext_t *uc=context;if(!uc->uc_mcontext)return 0;
    __typeof__(uc->uc_mcontext->__ss) *ss=&uc->uc_mcontext->__ss;
    uint64_t pc=ss->__pc;uint32_t trap;
    if(pc<28 || (pc&3) || !klj_x18_copy(pc,&trap,sizeof trap) ||
       (trap&0xffe0001fu)!=0xd4400000u)return 0;
    unsigned immediate=(trap>>5)&0xffffu;
    unsigned tag=immediate&0xfc00u;
    if(tag!=0x6800u && tag!=0x6c00u)return 0;
    unsigned a=(immediate>>5)&31u,b=immediate&31u;
    if(a>=29 || b>=29 || a==b || a==18 || b==18)return 0;
    uint32_t words[10],plain;
    if(!klj_x18_copy(pc-28,words,(tag==0x6800u?9:10)*sizeof *words))return 0;
    if(words[0]!=0xd10083ffu || words[1]!=enc_stp(a,b,0) ||
       words[2]!=enc_mrs_tpidrro(a) ||
       (words[3]&~(0xfffu<<10))!=enc_ldr(b,a,0) ||
       words[4]!=(0x910083e0u|a) ||
       words[7]!=trap || (words[8]&0xfc000000u)!=0x14000000u)return 0;
    if(tag==0x6800u) {
        uint32_t original=(words[5]&~(31u<<5))|(31u<<5)|(18u<<16)|(1u<<23);
        if(((words[5]>>5)&31u)!=a || words[6]!=(0x8b000000u|(b<<16)|(a<<5)|b) ||
           !klj_stack_simd(original,&plain) || words[5]!=((plain&~(31u<<5))|(a<<5)))return 0;
    } else if(words[6]!=0xd503201fu || !klj_stack_alu(words[9],a,b,&plain) || words[5]!=plain)return 0;
    uint64_t saved[2];
    if(ss->__sp>UINT64_MAX-32 || ss->__x[a]!=ss->__sp+32 ||
       !klj_x18_copy(ss->__sp,saved,sizeof saved))return 0;
    int64_t displacement=(int64_t)((words[8]&0x03ffffffu)^0x02000000u)-0x02000000;
    uint64_t next=pc+4+(uint64_t)(displacement*4),sp=ss->__x[b];
    ss->__x[a]=saved[0];ss->__x[b]=saved[1];ss->__sp=sp;ss->__pc=next;
    return 1;
}

int klx_stack_resume(void *context) {
    int saved=errno,result=klj_x18_stack_resume(context);errno=saved;return result;
}
