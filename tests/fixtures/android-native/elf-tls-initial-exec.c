#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {uintptr_t module,offset;} Index;
static void (*enter)(void);
static void *(*pointer)(void);
static int *(*address)(void),*(*hidden)(void),*(*dependency)(void),*(*weak)(void);
static int *(*peer_address)(void),*(*peer_dependency)(void);
static unsigned char *(*zero)(void),*(*peer_zero)(void);
static void *(*guest_pointer)(void);
static void *(*tls_address)(const Index *);
static uintptr_t (*tls_register)(const void *,size_t,size_t,size_t,size_t);
static void (*tls_unregister)(uintptr_t);
static int (*static_offset)(const Index *,uintptr_t *);
static void *(*symbol)(void *,const char *);
static void *provider,*guest;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static unsigned arrived,checked;
static int loaded;
static int *addresses[3];
static intptr_t tp_offset;
static pthread_key_t late_key;
static unsigned late_callbacks;

static void check(unsigned index,int initial_dependency){
    assert(pointer()==guest_pointer());
    assert(*address()==37&&*hidden()==11&&*dependency()==initial_dependency);
    assert(weak()==pointer()); /* Original Bionic weak initial-exec semantics. */
    assert(address()==symbol(guest,"ie_initialized")&&address()==peer_address());
    assert(dependency()==symbol(provider,"tls_dependency")&&dependency()==peer_dependency());
    assert(zero()==peer_zero()&&!((uintptr_t)zero()&255));
    for(unsigned n=0;n<73;n++)assert(zero()[n]==0);
    assert((char *)address()-(char *)pointer()==tp_offset);
    addresses[index]=address();*address()=100+(int)index;zero()[72]=(unsigned char)(index+1);
}
static void late(void *opaque){
    unsigned index=(unsigned)(uintptr_t)opaque-1;
    assert(pointer()==guest_pointer()&&address()==addresses[index]&&*address()==100+(int)index);
    assert(zero()[72]==index+1&&peer_address()==address());
    pthread_mutex_lock(&lock);++late_callbacks;pthread_mutex_unlock(&lock);
}
static void *worker(void *opaque){
    unsigned index=(unsigned)(uintptr_t)opaque;
    enter();void *tp=pointer();uintptr_t guard=((uintptr_t *)tp)[5];
    int *before=symbol(provider,"tls_dependency");
    assert(before&&*before==83);*before=90+(int)index;
    if(index<2){
        pthread_mutex_lock(&lock);++arrived;pthread_cond_broadcast(&changed);
        while(!loaded)pthread_cond_wait(&changed,&lock);
        pthread_mutex_unlock(&lock);
    }
    /* No second guest entry or TLS descriptor call initializes the new IE
     * module: registration must already have populated this old thread. */
    assert(pointer()==tp&&((uintptr_t *)tp)[5]==guard);
    check(index,90+(int)index);
    assert(!pthread_setspecific(late_key,(void *)(uintptr_t)(index+1)));
    // All three allocations must still be live when their addresses are compared.
    pthread_mutex_lock(&lock);++checked;pthread_cond_broadcast(&changed);
    while(checked<3)pthread_cond_wait(&changed,&lock);
    pthread_mutex_unlock(&lock);
    return NULL;
}
int main(int argc,char **argv){
    assert(argc==6);void *core=dlopen(argv[1],RTLD_NOW|RTLD_GLOBAL);
    if(!core){fprintf(stderr,"%s\n",dlerror());return 2;}
    void *(*load)(const char *)=dlsym(core,"kl_load");symbol=dlsym(core,"kl_sym");
    const char *(*error)(void)=dlsym(core,"kl_error");
    void (*publish)(const char *,void *)=dlsym(core,"kl_register_image");
    enter=dlsym(core,"kl_thread_init");pointer=dlsym(core,"klj_guest_thread_pointer");
    int (*configure)(const char *)=dlsym(core,"klj_process_ids_configure");
    assert(load&&symbol&&error&&publish&&enter&&pointer&&configure);
    assert(configure(getenv("KL_ANDROID_ID_DIRECTORY"))>0);enter();
    provider=load(argv[2]);if(!provider){fprintf(stderr,"%s\n",error());return 3;}
    publish("libtls-dependency.so",provider);
    int *dependency_before=symbol(provider,"tls_dependency");assert(dependency_before&&*dependency_before==83);
    *dependency_before=109;
    assert(!pthread_key_create(&late_key,late));
    pthread_t threads[3];for(unsigned n=0;n<2;n++)assert(!pthread_create(&threads[n],NULL,worker,(void *)(uintptr_t)n));
    pthread_mutex_lock(&lock);while(arrived<2)pthread_cond_wait(&changed,&lock);pthread_mutex_unlock(&lock);
    guest=load(argv[3]);if(!guest){fprintf(stderr,"%s\n",error());return 4;}
    publish("libtls-initial-exec.so",guest);
    void *peer=load(argv[4]);if(!peer){fprintf(stderr,"%s\n",error());return 5;}
    address=symbol(guest,"ie_address");hidden=symbol(guest,"ie_hidden_address");
    dependency=symbol(guest,"ie_dependency_address");weak=symbol(guest,"ie_weak_address");
    zero=symbol(guest,"ie_zero_address");guest_pointer=symbol(guest,"ie_thread_pointer");
    peer_address=symbol(peer,"peer_address");peer_zero=symbol(peer,"peer_zero_address");
    peer_dependency=symbol(peer,"peer_dependency_address");
    assert(address&&hidden&&dependency&&weak&&zero&&guest_pointer&&peer_address&&peer_zero&&peer_dependency);
    assert(dependency()==dependency_before&&*dependency()==109);
    tp_offset=(char *)address()-(char *)pointer();assert(tp_offset>=64);
    assert(*address()==37);*address()=47;zero()[0]=7;
    pthread_mutex_lock(&lock);loaded=1;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&lock);
    assert(!pthread_create(&threads[2],NULL,worker,(void *)(uintptr_t)2));
    for(unsigned n=0;n<3;n++)assert(!pthread_join(threads[n],NULL));
    assert(late_callbacks==3&&addresses[0]!=addresses[1]&&addresses[1]!=addresses[2]);
    assert(*address()==47&&zero()[0]==7&&*dependency()==109);
    /* Exhausted/overaligned modules keep dynamic TLS instead of fabricating a
     * constant offset. The image loader diagnoses an actual IE requirement. */
    assert(!load(argv[5]));assert(strstr(error(),"initial-exec TLS"));
    tls_register=dlsym(core,"klj_elf_tls_register");tls_unregister=dlsym(core,"klj_elf_tls_unregister");
    tls_address=dlsym(core,"klj_elf_tls_address");static_offset=dlsym(core,"klj_elf_tls_static_offset");
    assert(tls_register&&tls_unregister&&tls_address&&static_offset);
    int value=19;uintptr_t offset=0;
    uintptr_t module=tls_register(&value,sizeof value,32,65536,17);assert(module);
    Index index={module,0};assert(!static_offset(&index,&offset));
    void *skew=tls_address(&index);int copied=0;memcpy(&copied,skew,sizeof copied);
    assert(((uintptr_t)skew&65535)==17&&copied==19);
    assert((uintptr_t)skew-(uintptr_t)pointer()==offset);tls_unregister(module);
    uintptr_t next=tls_register(&value,sizeof value,32,65536,17);assert(next&&next!=module);
    index.module=next;uintptr_t next_offset=0;assert(!static_offset(&index,&next_offset)&&next_offset!=offset);
    memcpy(&copied,skew,sizeof copied);assert(copied==19);tls_unregister(next);
    module=tls_register(&value,sizeof value,1024*1024+1,64,0);assert(module);index.module=module;
    errno=0;assert(static_offset(&index,&offset)<0&&errno==ENOSPC);assert(*(int *)tls_address(&index)==19);tls_unregister(module);
    module=tls_register(&value,sizeof value,32,131072,0);assert(module);index.module=module;
    assert(static_offset(&index,&offset)<0);assert(!((uintptr_t)tls_address(&index)&131071));tls_unregister(module);
    pid_t child=fork();assert(child>=0);
    if(!child){enter();_exit(*address()==47&&zero()[0]==7&&*dependency()==109&&peer_address()==address()?0:6);}
    int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    puts("PASS original initial-exec ELF TLS shares real TP-relative storage with dynamic TLS on existing/new threads, late destructors and fork");
}
