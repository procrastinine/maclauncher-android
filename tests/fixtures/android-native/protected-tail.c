#include <assert.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void (*enter)(void);
static uintptr_t (*run)(uintptr_t);
static uintptr_t (*spilled)(uintptr_t);
static void *worker(void *unused){
    (void)unused;enter();assert(run(7)==0x45d);assert(spilled(7)==0x45d);return NULL;
}
int main(int argc,char **argv){
    assert(argc==4);void *core=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if(!core){puts(dlerror());return 2;}
    int (*configure)(const char *)=dlsym(core,"klj_process_ids_configure");
    void *(*load)(const char *)=dlsym(core,"kl_load");
    void *(*symbol)(void *,const char *)=dlsym(core,"kl_sym");
    const char *(*error)(void)=dlsym(core,"kl_error");
    enter=dlsym(core,"kl_thread_init");assert(configure&&load&&symbol&&error&&enter);
    assert(configure(argv[3])>0);enter();
    void *image=load(argv[2]);if(!image){puts(error());return 3;}
    const uint32_t *entry=symbol(image,"protected_entry");assert(entry);
    const uint32_t prefix[]={0x5281807c,0x7a83a87d,0x2a85d475};
    assert(!memcmp(entry,prefix,sizeof prefix));
    assert(entry[4]!=0xd53bd053u); // Actual Android TP veneer replaces MRS.
    const uint32_t constants[]={0xd53bd053,0xf9401668,0xf9000be8,0xd65f03c0};
    assert(!memcmp(symbol(image,"protected_constants"),constants,sizeof constants));
    run=(void *)(entry+3);assert(run(5)==0x45b);
    const uint32_t *spill=symbol(image,"protected_spilled_entry");assert(spill);
    assert(!memcmp(spill,prefix,sizeof prefix));assert(spill[7]!=0xd53bd048u);
    spilled=(void *)(spill+3);assert(spilled(5)==0x45b);
    const uint32_t *opaque=symbol(image,"protected_opaque_continuation");assert(opaque);
    assert(opaque[5]!=0xd53bd056u);
    assert(opaque[7]==0x7a839c6bu&&opaque[8]==0x2a81fc7cu);
    pthread_t thread;assert(!pthread_create(&thread,NULL,worker,NULL));
    assert(!pthread_join(thread,NULL));assert(run(9)==0x45f);assert(spilled(9)==0x45f);
    puts("PASS actual ELF continuation executes Android TP/canary and x18 on two threads with opaque prefix and data bytes preserved");
}
