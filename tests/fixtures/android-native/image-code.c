#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "klepton.h"
#include "image-elf-types.h"
#include "image-code-ranges.h"
#include KL_X18_SOURCE
#include "image-tls-source.h"
#include "inline-syscall.h"

/* This fixture inspects/rewrites bytes; it never executes a guest syscall. */
void klj_inline_syscall_enter(void){assert(0);}
long klb_syscall(long n,long a,long b,long c,long d,long e,long f){(void)n;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;assert(0);return -1;}
int kl_errno_to_linux(int value){return value;}
static int (*pointer_offset)(void);
static int64_t (*pointer_stub)(uint32_t,uint32_t,uint32_t,uint32_t *,size_t);
int klj_guest_thread_pointer_offset(void){return pointer_offset();}
int64_t klj_guest_thread_pointer_stub(uint32_t word,uint32_t offset,uint32_t shadow,uint32_t *out,size_t capacity){return pointer_stub(word,offset,shadow,out,capacity);}
static void pattern(uint32_t *words,int valid,int indexed){
    for(unsigned i=0;i<40;i++)words[i]=i<12?0x00010203u:0xd503201fu;
    // Canonical callee-saved frame prefix with an executable unsectioned tail.
    words[12]=0xfc170fecu;words[13]=0x6d012bebu;words[14]=0x6d0223e9u;
    words[15]=indexed?0xa9be7bfdu:0xa9037bfdu;
    words[16]=indexed?0x910003fdu:valid?0x9100c3fdu:0x910083fdu;
    words[24]=0xd53bd048u;words[25]=0xaa0003f2u;words[26]=0xd4000001u;
}
int main(int argc,char **argv){
    assert(argc==2);void *runtime=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if(!runtime){puts(dlerror());return 1;}
    pointer_offset=dlsym(runtime,"klj_guest_thread_pointer_offset");
    pointer_stub=dlsym(runtime,"klj_guest_thread_pointer_stub");assert(pointer_offset&&pointer_stub);
    uint8_t file[4096]={0};Elf64_Ehdr *eh=(void *)file;
    eh->e_ehsize=sizeof *eh;eh->e_phoff=sizeof *eh;eh->e_phnum=2;eh->e_phentsize=sizeof(Elf64_Phdr);
    eh->e_shoff=0x100;eh->e_shnum=3;eh->e_shentsize=sizeof(Elf64_Shdr);
    Elf64_Phdr *ph=(void *)(file+eh->e_phoff);
    ph[0]=(Elf64_Phdr){.p_type=PT_LOAD,.p_flags=PF_X|PF_R,.p_filesz=sizeof file,.p_memsz=sizeof file};
    ph[1]=(Elf64_Phdr){.p_type=0x6474e550,.p_offset=0x300,.p_vaddr=0x300,.p_filesz=20};
    Elf64_Shdr *sh=(void *)(file+eh->e_shoff);
    sh[1]=(Elf64_Shdr){.sh_type=SHT_PROGBITS,.sh_flags=2|SHF_EXECINSTR,.sh_addr=0x400,.sh_offset=0x400,.sh_size=0x40};
    sh[2]=(Elf64_Shdr){.sh_type=SHT_PROGBITS,.sh_flags=2,.sh_addr=0xa00,.sh_offset=0xa00,.sh_size=0x100};
    // One original unwind-header function boundary deliberately starts in
    // padding. A verified frame record further bounds the local instruction vote.
    file[0x300]=1;file[0x301]=3;file[0x302]=3;file[0x303]=3;
    uint32_t header[]={0x340,1,0x800,0x350};memcpy(file+0x304,header,sizeof header);
    uint32_t cie[]={4,0},fde[]={4,0x14};memcpy(file+0x340,cie,sizeof cie);memcpy(file+0x350,fde,sizeof fde);
    pattern((void *)(file+0x800),1,0);pattern((void *)(file+0xa00),1,0);
    pattern((void *)(file+0xc00),0,0);pattern((void *)(file+0xe00),1,1);
    KlImageCodeMetadata metadata={0};assert(klj_code_metadata_read(&metadata,file,sizeof file,eh,ph,sh));
    klx_code_context context={metadata.marks,metadata.count,0x800,NULL,0};
    assert(kl_x18_is_data(file+0x800,160,24));
    assert(!kl_x18_is_data_context(file+0x800,160,24,&context));
    klx_code_context invalid={metadata.marks,metadata.count,0xc00,NULL,0};
    assert(kl_x18_is_data_context(file+0xc00,160,24,&invalid));
    klx_code_context indexed={metadata.marks,metadata.count,0xe00,NULL,0};
    assert(!kl_x18_is_data_context(file+0xe00,160,24,&indexed));
    KlImageCodeCursor cursor={.metadata=&metadata};KlImageCodeRange range;int tail=0;
    while(klj_image_code_next(eh,ph,sh,sizeof file,&cursor,&range)){
        assert(!(range.address<=0xa60 && range.address+range.size>0xa60));
        if(range.address<=0x860&&range.address+range.size>0x860)tail=1;
    }assert(tail);
    uint32_t unchanged[40],code[40],pool[40*KLX_VEN_MAX_INSN];
    memcpy(unchanged,file+0x800,sizeof unchanged);memcpy(code,unchanged,sizeof code);
    kl_stats plain={0},typed={0};rewrite_tls((void *)unchanged,sizeof unchanged,0x800,"fixture",&plain,NULL,NULL,NULL);
    rewrite_tls((void *)code,sizeof code,0x800,"fixture",&typed,&context,&metadata,NULL);
    assert(plain.tls_refused==1&&!plain.tls_rewrites&&unchanged[24]==0xd53bd048u);
    assert(typed.tls_rewrites==1&&!typed.tls_refused&&code[24]==0xd53bd048u);
    // Proven original metadata is accounted as data, never as a refused
    // executable TP; the same uncertain bytes without evidence still refuse.
    KlImageDataRange known_data={0x800,0x800+sizeof unchanged};
    KlImageCodeMetadata known={.data=&known_data,.dataCount=1};
    kl_stats excluded={0};size_t data_words=0;
    rewrite_tls((void *)unchanged,sizeof unchanged,0x800,"fixture",&excluded,NULL,&known,&data_words);
    assert(data_words==1&&!excluded.tls_refused&&!excluded.tls_rewrites);
    assert(unchanged[24]==0xd53bd048u);
    unsigned data,ctr,want=klj_x18_survey_context(code,sizeof code,&data,&ctr,&context);kl_x18_stats stats;size_t used=0;
    assert(!klj_x18_emit_surveyed(code,sizeof code,0x800,pool,sizeof pool,0x2000,&stats,&used,want,data,ctr,&context));
    assert(stats.patched&&code[25]!=0xaa0003f2u);
    assert(klj_inline_syscall_count(file+0x800,160)==0&&klj_inline_syscall_count_context(file+0x800,160,&context)==2);
    size_t page=(size_t)getpagesize();uint8_t *mapping=mmap(NULL,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);assert(mapping!=MAP_FAILED);
    memcpy(mapping,file+0x800,160);used=0;
    assert(!klj_inline_syscall_patch_context(mapping,160,mapping+page,page,&used,&context)&&used==page);
    assert((((uint32_t *)mapping)[26]&0xfc000000u)==0x14000000u);
    assert((((uint32_t *)mapping)[24]&0xfc000000u)==0x14000000u);
    assert(!memcmp(mapping,file+0x800,24*4)&&((uint32_t *)mapping)[25]==((uint32_t *)(file+0x800))[25]);munmap(mapping,page*2);
    klj_code_metadata_release(&metadata);assert(!metadata.count&&!metadata.dataCount&&!metadata.marks);
    puts("PASS shared ELF function/unwind/frame context: TLS, x18, SVC, typed data and unsectioned code");
}
