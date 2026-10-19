// Owned adapter around the pinned, unchanged Klepton source.
// Bionic libc's allocation operators use malloc/free and terminate on failure.
// A guest C++ runtime remains authoritative when it supplies these operators.
#ifndef KL_SHIM_IMPLEMENTATION
#error Define the path to the unchanged Klepton shim implementation
#endif
#define kl_shim_lookup kl_upstream_shim_lookup
#include KL_SHIM_IMPLEMENTATION
#undef kl_shim_lookup
#include <stdatomic.h>
#include <stdio.h>
#include "android-properties.h"
#include "native-imports.h"
#include "native-file-imports.h"
#include "directory-bridge.h"
#include "pollable-fds/bridge.h"
#include "native-libc-imports.h"
#include "native-cxx-imports.h"
#include "native-resolver-imports.h"
#include "native-socket-imports.h"
#include "native-zlib-imports.h"
#include "process-identity.h"
#include "signal-bridge.h"
#include "native-process-imports.h"
#include "process-abi/prctl.h"
#include "native-memory-trace.h"
#include "executable-memory/jvm.h"
#include "image-unwind.h"
#include <kl_va.h>
// The nonvariadic entry receives the same AAPCS va_list used by the existing
// syslog thunk. Formatting and logging retain that one implementation.
extern int klh_syslog(int,const char *,kl_va *);
extern int klh_sscanf(const char *,const char *,kl_va *);
extern int klh_fscanf(void *,const char *,kl_va *);
extern int kl_mmap_flags(int);
static void klj_vsyslog(int priority,const char *format,kl_va *arguments){(void)klh_syslog(priority,format,arguments);}
// The native log sink owns all Android buffers. Preserve its actual write
// result and priority/tag/text rather than adding another logging service.
static int klj_android_log_buf_write(int buffer,int priority,const char *tag,const char *text){
    (void)buffer;return kl_android_log_write(priority,tag,text);
}
// Keep guest mapping mutations serialized with the multi-syscall Mach remap.
// Actual allocation, JIT refusal, virtual OBB handling and flags stay Klepton's.
static void *klj_guest_mmap(void *address,size_t length,int protection,int flags,int fd,off_t offset){
    void *(*call)(void *,size_t,int,int,int,off_t)=kl_upstream_shim_lookup("mmap");
    pthread_mutex_lock(&klj_vm_lock);
    int native_flags=kl_mmap_flags(flags);
    void *result=(native_flags&MAP_ANON)&&(native_flags&MAP_PRIVATE)&&(protection&PROT_EXEC)?
        klj_executable_map(address,length,protection,native_flags,-1,offset):
        call(address,length,protection,flags,fd,offset);
    int error=errno;
    if(result!=MAP_FAILED&&(native_flags&MAP_FIXED))klj_executable_code_forget(result,length);
    klj_vm_trace("mmap",result==MAP_FAILED?address:result,length,protection,
        result==MAP_FAILED?-1:0,result==MAP_FAILED?error:0,__builtin_return_address(0));
    pthread_mutex_unlock(&klj_vm_lock);errno=error;return result;
}
static int klj_guest_munmap(void *address,size_t length){
    pthread_mutex_lock(&klj_vm_lock);int result=munmap(address,length),error=errno;
    if(!result)klj_executable_code_forget(address,length);
    pthread_mutex_unlock(&klj_vm_lock);errno=error;return result;
}
static int klj_guest_mprotect(void *address,size_t length,int protection){
    pthread_mutex_lock(&klj_vm_lock);int result=klj_executable_protect(address,length,protection),error=errno;
    klj_vm_trace("mprotect",address,length,protection,result,result?error:0,__builtin_return_address(0));
    pthread_mutex_unlock(&klj_vm_lock);errno=error;return result;
}
extern void klj_android_log_assert(void);
extern void *klj_math_lookup(const char *name);
extern void *klj_ctype_lookup(const char *name);
extern void *klj_terminal_lookup(const char *name);
extern void *klj_locale_lookup(const char *name);
extern void *klj_sort_lookup(const char *name);
extern void *klj_exec_lookup(const char *name);
extern void *klj_database_lookup(const char *name);
extern void *klj_wide_stdio_lookup(const char *name);
extern void *klj_allocator_lookup(const char *name);
extern void *klj_nonlocal_jump_lookup(const char *name);
extern void *klj_search_lookup(const char *name);
_Noreturn void klj_android_log_assert_message(const char *condition,const char *tag,const char *format,void *arguments) {
    if(format)((int (*)(int,const char *,const char *,void *))klh_android_log_print)(7,tag,format,arguments);
    else fprintf(stderr,"F/%s: Assertion failed: %s\n",tag?tag:"",condition?condition:"unspecified");
    abort();
}
typedef void *(*host_legacy_allocator)(size_t);
static _Atomic(host_legacy_allocator) host_legacy_allocate;
static _Noreturn void host_legacy_pure_virtual(void) {
    fputs("Android C++ pure virtual function called\n",stderr);
    abort();
}
static void *host_legacy_new(size_t bytes) {
    host_legacy_allocator allocate=atomic_load_explicit(&host_legacy_allocate,memory_order_acquire);
    void *result=allocate(bytes);
    if(!result){fprintf(stderr,"Android C++ allocation failed for %zu bytes\n",bytes);abort();}
    return result;
}
static void *klj_resolve_native_import(const char *name) {
    if(!strcmp(name,"vsyslog"))return (void*)klj_vsyslog;
    if(!strcmp(name,"__android_log_buf_write"))return (void *)klj_android_log_buf_write;
    if(!strcmp(name,"vsscanf"))return (void *)klh_sscanf;
    if(!strcmp(name,"vfscanf"))return (void *)klh_fscanf;
    if(!strcmp(name,"dl_unwind_find_exidx"))return (void *)dl_unwind_find_exidx;
    if(!strcmp(name,"prctl"))return (void *)klj_process_prctl;
    if(!strcmp(name,"poll"))return (void *)klj_poll;
    void *directory=klj_directory_lookup(name);
    if(directory)return directory;
    if(!strcmp(name,"scandir64"))return kl_upstream_shim_lookup("scandir");
    if(!strcmp(name,"alphasort64"))return kl_upstream_shim_lookup("alphasort");
    void *identity=klj_process_identity_symbol(name);
    if(identity)return identity;
    void *answer=klj_android_property_lookup(name);
    if(answer)return answer;
    // Resolver calls and __get_h_errno must share the same per-thread result;
    // the generic host forwarders expose Darwin's global error slot instead.
    answer=klj_resolver_lookup(name);
    if(answer)return answer;
    answer=klj_socket_lookup(name);
    if(answer)return answer;
    answer=klj_pollable_lookup(name);
    if(answer)return answer;
    answer=klj_native_file_lookup(name);
    if(answer)return answer;
    answer=klj_math_lookup(name);
    if(answer)return answer;
    answer=klj_ctype_lookup(name);
    if(answer)return answer;
    answer=klj_terminal_lookup(name);
    if(answer)return answer;
    answer=klj_locale_lookup(name);
    if(answer)return answer;
    answer=klj_sort_lookup(name);
    if(answer)return answer;
    answer=klj_exec_lookup(name);
    if(answer)return answer;
    answer=klj_signal_lookup(name);
    if(answer)return answer;
    answer=klj_process_abi_lookup(name);
    if(answer)return answer;
    answer=klj_database_lookup(name);
    if(answer)return answer;
    answer=klj_wide_stdio_lookup(name);
    if(answer)return answer;
    answer=klj_allocator_lookup(name);
    if(answer)return answer;
    answer=klj_nonlocal_jump_lookup(name);
    if(answer)return answer;
    answer=klj_search_lookup(name);
    if(answer)return answer;
    if(!strcmp(name,"mmap")||!strcmp(name,"mmap64"))return (void *)klj_guest_mmap;
    if(!strcmp(name,"munmap"))return (void *)klj_guest_munmap;
    if(!strcmp(name,"mprotect"))return (void *)klj_guest_mprotect;
    // Native ABI differences belong to their existing adapter family. Generic
    // same-ABI Darwin forwarding is a fallback, never an earlier competing owner.
    answer=klj_native_libc_lookup(name);
    if(answer)return answer;
    answer=kl_upstream_shim_lookup(name);
    if(answer)return answer;
    answer=klj_zlib_lookup(name);
    if(answer)return answer;
    // On ARM64 off_t is already 64 bit. Reuse Klepton's checked positioned
    // I/O, including its OBB descriptor ownership, for the unsuffixed names.
    if(!strcmp(name,"__pread_chk"))return kl_upstream_shim_lookup("__pread64_chk");
    if(!strcmp(name,"__pwrite_chk"))return kl_upstream_shim_lookup("__pwrite64_chk");
    if(!strcmp(name,"__android_log_assert"))return (void *)klj_android_log_assert;
    answer=klj_native_import_lookup(name);
    if(answer)return answer;
    if(!strncmp(name,"__cxa_guard_",12)){
        answer=kl_guest_sym_global(name);
        return answer?answer:klj_native_cxx_lookup(name);
    }
    if(!strcmp(name,"__cxa_pure_virtual")){
        answer=kl_guest_sym_global(name);
        return answer?answer:(void *)host_legacy_pure_virtual;
    }
    int deletion=!strcmp(name,"_ZdlPv")||!strcmp(name,"_ZdaPv");
    int allocation=!strcmp(name,"_Znwm")||!strcmp(name,"_Znam");
    if(!deletion&&!allocation)return NULL;
    answer=kl_guest_sym_global(name);
    if(answer)return answer;
    if(deletion)return kl_upstream_shim_lookup("free");
    host_legacy_allocator allocate=(host_legacy_allocator)kl_upstream_shim_lookup("malloc");
    if(!allocate)return NULL;
    atomic_store_explicit(&host_legacy_allocate,allocate,memory_order_release);
    return (void *)host_legacy_new;
}

typedef void *(*klj_native_import_adapter)(const char *,void *);
static _Atomic(klj_native_import_adapter) native_import_adapter;
void klj_set_native_import_adapter(klj_native_import_adapter adapter){
    atomic_store_explicit(&native_import_adapter,adapter,memory_order_release);
}
void *kl_shim_lookup(const char *name){
    void *target=klj_resolve_native_import(name);
    klj_native_import_adapter adapter=atomic_load_explicit(&native_import_adapter,memory_order_acquire);
    return target&&adapter?adapter(name,target):target;
}
