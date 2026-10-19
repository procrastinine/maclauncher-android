#include "jvm.h"
#include "api.h"
#include "../thread-identity.h"
#include "../signal-bridge.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include KLEPTON_TLS_DEFINES

extern int kl_errno_from_linux(int);
typedef struct {
    unsigned long long *cursor;
    uintptr_t low,high;
    unsigned int *in_host;
} NativeCall;
_Static_assert(offsetof(NativeCall,low)==8&&offsetof(NativeCall,high)==16&&
               offsetof(NativeCall,in_host)==24,"Native SVC anchor layout");
typedef struct {
    void *storage;
    void *stack;
    size_t stack_size;
    void *tls[BIONIC_TLS_SLOTS];
    void *elf_tls;
    void (*retire_elf_tls)(void *);
    NativeCall native_call;
} ExecutableThread;

static pthread_once_t pointer_once=PTHREAD_ONCE_INIT;
static pthread_key_t pointer_key,native_call_key;
/* Darwin's allocated TSD slot, consumed by the leaf SVC entry before C runs. */
unsigned long long klj_inline_call_slot_offset;
static int pointer_error;
static void allocate_pointer_key(void){
    pointer_error=pthread_key_create(&pointer_key,NULL);
    if(!pointer_error)pointer_error=pthread_key_create(&native_call_key,NULL);
    if(!pointer_error&&native_call_key>4095)pointer_error=EOVERFLOW;
    if(!pointer_error)klj_inline_call_slot_offset=(unsigned long long)native_call_key*8;
}
int klj_guest_thread_pointer_offset(void){
    pthread_once(&pointer_once,allocate_pointer_key);
    if(pointer_error){errno=pointer_error;return -1;}
    if(pointer_key>4095){errno=EOVERFLOW;return -1;}
    return (int)pointer_key*8;
}

static void retire_thread(void *opaque){
    ExecutableThread *thread=opaque;
    if(thread->elf_tls)thread->retire_elf_tls(thread->elf_tls);
    munmap(thread->stack,thread->stack_size);
    free(thread);
}

int klj_executable_thread_enter(void){
    // Darwin may retire compiler TLS before a later guest POSIX destructor.
    // Recover this live Mach thread's retained identity before borrowing its
    // execution context; it must not acquire a second context during cleanup.
    if(klj_guest_thread_pointer_offset()<0||klj_thread_id()<0)return -1;
    ExecutableThread *existing=klj_thread_memory_context();
    if(existing){
        int error=pthread_setspecific(native_call_key,&existing->native_call);
        if(error){errno=error;return -1;}
        return 0;
    }
    size_t size=klj_guest_executable_context_size(),alignment=klj_guest_executable_context_alignment();
    size_t offset=(sizeof(ExecutableThread)+alignment-1)&~(alignment-1);
    ExecutableThread *thread=NULL;
    int error=posix_memalign((void **)&thread,alignment,offset+size);
    if(error){errno=error;return -1;}
    memset(thread,0,sizeof *thread);
    size_t page=(size_t)getpagesize(),usable=65536;
    thread->storage=(char *)thread+offset;thread->stack_size=usable+2*page;
    thread->stack=mmap(NULL,thread->stack_size,PROT_NONE,MAP_ANON|MAP_PRIVATE,-1,0);
    if(thread->stack==MAP_FAILED){error=errno;free(thread);errno=error;return -1;}
    if(mprotect((char *)thread->stack+page,usable,PROT_READ|PROT_WRITE)){
        error=errno;retire_thread(thread);errno=error;return -1;
    }
    int64_t result=klj_guest_executable_context_init(thread->storage,size,(uintptr_t)thread->stack+page,usable);
    if(result<0){error=kl_errno_from_linux((int)-result);retire_thread(thread);errno=error;return -1;}
    klj_guest_executable_native_stack(thread->storage,&thread->native_call.cursor,&thread->native_call.in_host);
    thread->native_call.low=(uintptr_t)thread->stack+page;
    thread->native_call.high=thread->native_call.low+usable;
    error=pthread_setspecific(native_call_key,&thread->native_call);
    if(error){retire_thread(thread);errno=error;return -1;}
    if(klj_thread_memory_adopt(thread,retire_thread)){
        pthread_setspecific(native_call_key,NULL);
        error=errno;retire_thread(thread);errno=error;return -1;
    }
    return 0;
}

int klj_guest_thread_pointer_enter(unsigned long long canary){
    if(klj_guest_thread_pointer_offset()<0||klj_executable_thread_enter())return -1;
    ExecutableThread *thread=klj_thread_memory_context();
    void **pointer=thread->tls-MIN_TLS_SLOT;
    pointer[TLS_SLOT_STACK_GUARD]=(void *)(uintptr_t)canary;
    int error=pthread_setspecific(pointer_key,pointer);
    if(error){errno=error;return -1;}
    return 0;
}

void *klj_guest_thread_pointer(void){
    ExecutableThread *thread=klj_thread_memory_context();
    if(!thread){if(klj_executable_thread_enter())return NULL;thread=klj_thread_memory_context();}
    void **pointer=thread->tls-MIN_TLS_SLOT;
    if(pthread_getspecific(pointer_key)!=pointer){
        int error=pthread_setspecific(pointer_key,pointer);
        if(error){errno=error;return NULL;}
    }
    return pointer;
}

void *klj_executable_thread_tls(void *(*create)(void),void (*retire)(void *)){
    ExecutableThread *thread=klj_thread_memory_context();
    if(!thread){if(klj_executable_thread_enter())return NULL;thread=klj_thread_memory_context();}
    if(!thread->elf_tls){
        void *value=create();if(!value)return NULL;
        thread->retire_elf_tls=retire;thread->elf_tls=value;
    }
    return thread->elf_tls;
}

/* The original Bionic jump owner includes these reserved words in its own
 * checksum. A longjmp restores the exact suspended native cursor, including
 * nested callbacks, instead of depending on skipped C scope cleanup. */
void klj_executable_jump_save(uint64_t *saved){
    ExecutableThread *thread=klj_thread_memory_context();
    if(thread){saved[0]=(uintptr_t)thread;saved[1]=*thread->native_call.cursor;saved[2]=*thread->native_call.in_host;}
}
void klj_executable_jump_restore(const uint64_t *saved){
    ExecutableThread *thread=klj_thread_memory_context();
    if(thread&&saved[0]==(uintptr_t)thread){
        *thread->native_call.cursor=saved[1];
        *thread->native_call.in_host=(unsigned int)saved[2];
    }
}

int klj_executable_signal(int signal,int code,void *ucontext){
    ExecutableThread *thread=klj_thread_memory_context();
    return thread?klj_guest_executable_fault(thread->storage,signal,code,ucontext):0;
}

void klj_executable_signal_invoke(void *handler,int signal,const void *linux_info,void *ucontext){
    /* Original AIM builds/restores the Linux UAPI frame. The APK callback stays
     * entirely in C, so siglongjmp and native unwinding never cross Rust frames.
     * A nested signal gets its own native callback stack and frame storage. */
    size_t size=klj_guest_signal_frame_size();
    _Alignas(16) unsigned char storage[size];
    ucontext_t *native=ucontext;
    ExecutableThread *thread=klj_thread_memory_context();
    unsigned long long cursor=thread?*thread->native_call.cursor:0;
    unsigned int in_host=thread?*thread->native_call.in_host:0;
    uintptr_t interrupted=native->uc_mcontext->__ss.__sp;
    /* A handler can issue another SVC after switching to its own Go/Android
     * stack. Keep it below the interrupted C frame, including Darwin's red
     * zone, rather than reusing the outer syscall's still-live stack. */
    if(thread&&interrupted>thread->native_call.low+128&&interrupted<=thread->native_call.high)
        *thread->native_call.cursor=interrupted-128;
    KljGuestSignalFrame frame;
    uint64_t mask=klj_signal_mask_to_guest(native->uc_sigmask);
    if(klj_guest_signal_frame_prepare(storage,size,linux_info,ucontext,mask,&frame)){
        if(thread)*thread->native_call.cursor=cursor;
        return;
    }
    if(thread)*thread->native_call.in_host=0;
    ((void (*)(int,void *,void *))handler)(signal,frame.info,frame.context);
    if(!klj_guest_signal_frame_restore(frame.frame,ucontext,&mask))
        native->uc_sigmask=klj_signal_mask_to_host(mask);
    if(thread){*thread->native_call.cursor=cursor;*thread->native_call.in_host=in_host;}
}

void klj_executable_signal_chain(void *handler,int flags,int signal,const void *linux_info,void *linux_context){
    int saved=errno,host=klj_signal_to_host(signal);
    if(host<=0)return;
    if(!(flags&SA_SIGINFO)){
        ((void (*)(int))handler)(host);errno=saved;return;
    }
    /* A saved HotSpot disposition is a Darwin callback. Reconstruct that ABI
     * from the caller's Linux context rather than exposing its native address.
     * Stack-owned conversion also survives nesting and siglongjmp without a
     * second thread-local callback owner. */
    ucontext_t native={0};struct __darwin_mcontext64 machine={0};
    native.uc_mcontext=&machine;native.uc_mcsize=sizeof machine;
    uint64_t mask=0;
    if(linux_context){
        if(klj_guest_signal_context_restore(linux_context,&native,&mask))return;
        native.uc_sigmask=klj_signal_mask_to_host(mask);
    }
    siginfo_t info={0};const klj_signal_info *guest=linux_info;
    if(guest){
        info.si_signo=host;info.si_errno=kl_errno_from_linux(guest->error);info.si_code=guest->code;
        switch(guest->code){case 0:case -6:info.si_code=SI_USER;break;case -1:info.si_code=SI_QUEUE;break;
            case -2:info.si_code=SI_TIMER;break;case -3:info.si_code=SI_MESGQ;break;case -4:info.si_code=SI_ASYNCIO;break;}
        if(guest->code<=0||host==SIGCHLD){
            info.si_pid=guest->data.sender.pid;info.si_uid=guest->data.sender.uid;
            info.si_value.sival_ptr=(void *)(uintptr_t)guest->data.sender.value;
            if(host==SIGCHLD){info.si_status=guest->data.sender.status;
                if(guest->code==CLD_KILLED||guest->code==CLD_DUMPED||guest->code==CLD_STOPPED||guest->code==CLD_CONTINUED||guest->code==CLD_TRAPPED)
                    info.si_status=klj_signal_to_host(info.si_status);}
        }else if(host==SIGIO)info.si_band=guest->data.poll.band;
        else info.si_addr=guest->data.fault.address;
    }
    ((void (*)(int,siginfo_t *,void *))handler)(host,guest?&info:NULL,linux_context?&native:NULL);
    if(linux_context)klj_guest_signal_context_update(linux_context,&native,klj_signal_mask_to_guest(native.uc_sigmask));
    errno=saved;
}

int klj_executable_signal_action(int signal,const void *action,void *previous){
    typedef struct {int32_t flags,pad;void *handler;uint64_t mask;void *restorer;} BionicAction;
    const BionicAction *in=action;BionicAction *out=previous;
    KljGuestSignalAction requested={0},old={0};
    if(in)requested=(KljGuestSignalAction){(uintptr_t)in->handler,(uint32_t)in->flags,(uintptr_t)in->restorer,in->mask};
    int64_t result=klj_guest_signal_action(signal,in?&requested:NULL,out?&old:NULL);
    if(result<0){errno=kl_errno_from_linux((int)-result);return -1;}
    if(out)*out=(BionicAction){.flags=(int32_t)old.flags,.handler=(void *)old.handler,.mask=old.mask,.restorer=(void *)old.restorer};
    return 0;
}

#include "code-publication.c"

int klj_executable_protect(void *address,size_t length,int protection){
    if((protection&PROT_EXEC)&&(klj_executable_thread_enter()||klj_executable_signal_enable()))return -1;
    int64_t result=klj_guest_executable_protect_prepared((uintptr_t)address,length,protection,prepare_executable_code,finish_executable_code);
    if(result<0){errno=kl_errno_from_linux((int)-result);return -1;}
    return 0;
}

void *klj_executable_map(void *address,size_t length,int protection,int native_flags,int descriptor,long long offset){
    /* Let the existing Darwin/OBB owner handle ordinary mappings. Executable
     * anonymous/private memory starts readable/writable, then enters the
     * original AIM JIT owner with the same bytes and requested permissions. */
    void *mapped=mmap(address,length,PROT_READ|PROT_WRITE,native_flags,descriptor,(off_t)offset);
    if(mapped==MAP_FAILED)return MAP_FAILED;
    if(klj_executable_protect(mapped,length,protection)){
        int error=errno;munmap(mapped,length);errno=error;return MAP_FAILED;
    }
    return mapped;
}
