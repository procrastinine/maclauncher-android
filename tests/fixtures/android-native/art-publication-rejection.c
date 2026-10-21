#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void) {
    size_t size=(size_t)sysconf(_SC_PAGESIZE);
    uint32_t *code=mmap(NULL,size,PROT_READ|PROT_WRITE|PROT_EXEC,
                        MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(code!=MAP_FAILED);
    const uint32_t original=0xd71f0a40; /* BRAA X18,X0: authenticated terminal refusal. */
    for(size_t k=0;k<4;k++)code[k]=0xd503201f;
    code[4]=original;code[5]=0xd65f03c0;
    /* These bytes are never executed. Rejected publication must preserve the
     * existing mapping, its write permission, and every original word. */
    for(unsigned k=0;k<10;k++) {
        errno=0;assert(mprotect(code,size,PROT_READ|PROT_EXEC)==-1);
        assert(errno==ENOEXEC);
        for(size_t i=0;i<4;i++)assert(code[i]==0xd503201f);
        assert(code[4]==original&&code[5]==0xd65f03c0);
        code[6]=k;
    }
    printf("PASS rejected original Bionic publication base=%p site=%p size=%#zx original=%08x attempts=10 errno=%d\n",
           (void *)code,(void *)(code+4),size,original,ENOEXEC);
    code[4]=0xaa0003f2; /* MOV X18,X0: successful final publication. */
    assert(!mprotect(code,size,PROT_READ|PROT_EXEC));
    assert((code[4]&0xfc000000u)==0x14000000u&&code[5]==0xd65f03c0);
    printf("PASS prepared original Bionic publication base=%p size=%#zx x18=1\n",(void *)code,size);
    assert(!munmap(code,size));
    code=mmap(NULL,size,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(code!=MAP_FAILED);code[0]=0xd503201f;code[1]=0xd65f03c0;
    assert(!mprotect(code,size,PROT_READ|PROT_EXEC));
    assert(code[0]==0xd503201f&&code[1]==0xd65f03c0);
    printf("PASS prepared original Bionic publication base=%p size=%#zx x18=0\n",(void *)code,size);
    assert(!munmap(code,size));return 0;
}
