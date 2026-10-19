import fs from 'node:fs';
import path from 'node:path';
const header='#include '+JSON.stringify(path.join(import.meta.dirname,'signal-bridge.h'))+'\n#include '+JSON.stringify(path.join(import.meta.dirname,'executable-memory/jvm.h'))+'\n#include <stdatomic.h>\n';
function replace(source,from,to){if(source.split(from).length!==2)throw Error('Upstream signal boundary changed: '+from);return source.replace(from,to);}
/** Adapt handler registration by ABI; one-argument handlers receive no ucontext. */
export function adaptSignalSource(source) {
  const refusal=source.indexOf('    static int allow_guest = -1;');
  const registration=source.indexOf('    if (in) {',refusal);
  if(refusal<0||registration<0)throw Error('Upstream signal registration boundary changed');
  source=source.slice(0,refusal)+source.slice(registration);
  const start = source.indexOf('// A guest handler for a *fatal* signal');
  const end = source.indexOf('static int klb_fatal_signal', start);
  if (start < 0 || end < 0) throw new Error('Upstream signal ABI boundary changed');
  source=source.slice(0, start) + `// One-argument POSIX handlers share the ARM64 calling convention and do not
// inspect a Linux context. Register them normally, including CPU feature probes.
// Saved Darwin dispositions are exposed only through a Linux ABI chain bridge.
// Guest SA_SIGINFO handlers still require Linux siginfo/ucontext
// translation through the original Linux frame owner.
` + source.slice(end);
  source=replace(source,'static struct { void *handler; int siginfo; } g_guest_sig[KL_NSIG];','static struct { void *handler; void *restorer; int siginfo; uint32_t flags; struct sigaction native_action; int native_saved, native_only; } g_guest_sig[KL_NSIG];\nstatic _Atomic int klj_executable_mux_active;\nstatic pthread_mutex_t klj_executable_mux_lock=PTHREAD_MUTEX_INITIALIZER;');
  source=replace(source,'static void kl_sig_x18_tramp(int sig, siginfo_t *info, void *uctx) {',`// This callable value has the guest ABI. Native handler pointers never escape
// through sigaction, including queries made before the first guest install.
static void kl_sig_native_chain(int sig, void *info, void *context) {
    int host = klj_signal_to_host(sig);
    if (host <= 0 || host >= KL_NSIG || !g_guest_sig[host].native_saved) return;
    struct sigaction native = g_guest_sig[host].native_action;
    if (native.sa_handler == SIG_IGN) return;
    if (native.sa_handler == SIG_DFL) {
        sigaction(host, &native, NULL); raise(host); return;
    }
    if (native.sa_flags & SA_RESETHAND) {
        g_guest_sig[host].native_action.sa_handler=SIG_DFL;
        if (g_guest_sig[host].native_only) g_guest_sig[host].handler=(void *)SIG_DFL;
    }
    klj_executable_signal_chain((void *)native.sa_handler, native.sa_flags, sig, info, context);
}
static void kl_sig_x18_tramp(int sig, siginfo_t *info, void *uctx) {`);
  source=replace(source,'static void kl_sig_x18_tramp(int sig, siginfo_t *info, void *uctx) {',`static void kl_sig_x18_tramp(int sig, siginfo_t *info, void *uctx) {
    if (sig == SIGILL && uctx && info &&
        klj_signal_is_hardware_fault(sig, info) && klx_stack_resume(uctx)) return;
    if (uctx && info && klj_signal_is_hardware_fault(sig, info) &&
        klj_executable_signal(sig, info->si_code, uctx)) return;
    // A process handler also sees HotSpot's implicit null/guard-page faults.
    // Their interrupted instruction belongs to the VM, not to the APK which
    // happened to install Crashlytics. Preserve the original native handler and
    // Darwin context; an ARM32 callback must never receive a JVM fault.
    if (sig > 0 && sig < KL_NSIG && g_guest_sig[sig].native_saved &&
        (g_guest_sig[sig].native_only ||
         (klb_fatal_signal(sig) && uctx && klj_signal_is_hardware_fault(sig, info)))) {
        ucontext_t *uc = (ucontext_t *)uctx;
        if (g_guest_sig[sig].native_only || !kl_addr_image((void *)(uintptr_t)uc->uc_mcontext->__ss.__pc, NULL)) {
            const struct sigaction native = g_guest_sig[sig].native_action;
            if (native.sa_handler != SIG_DFL && native.sa_handler != SIG_IGN) {
                if (native.sa_flags & SA_RESETHAND) {
                    g_guest_sig[sig].native_action.sa_handler=SIG_DFL;
                    if(g_guest_sig[sig].native_only)g_guest_sig[sig].handler=(void *)SIG_DFL;
                }
                if (native.sa_flags & SA_SIGINFO) native.sa_sigaction(sig, info, uctx);
                else native.sa_handler(sig);
                return;
            }
            // Default and ignored dispositions belong to the same native
            // execution domain. Falling through would invoke a guest crash
            // callback inside the VM/JIT which raised the native fault.
            if (native.sa_handler == SIG_IGN) return;
            sigaction(sig, &native, NULL);
            raise(sig);
            return;
        }
    }`);
  source=replace(source,'if (si) ((void (*)(int, siginfo_t *, void *))h)(sig, info, uctx);\n        else    ((void (*)(int))h)(sig);',`if (si) {
            klj_signal_info guest_info; klj_signal_info_to_guest(info,&guest_info);
            if (info && uctx && kl_addr_image(h,NULL))
                klj_executable_signal_invoke(h,klj_signal_to_guest(sig),&guest_info,uctx);
            else ((void (*)(int, void *, void *))h)(klj_signal_to_guest(sig), info ? &guest_info : NULL, uctx);
        } else ((void (*)(int))h)(klj_signal_to_guest(sig));`);
  source=replace(source,'    if (h) {',`    if (h == (void *)SIG_IGN) return;
    if (h == (void *)SIG_DFL) {
        struct sigaction action;
        memset(&action, 0, sizeof action); action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask); sigaction(sig, &action, NULL); raise(sig);
        return;
    }
    if (h) {`);
  source=replace(source,'    if (h) {',`    if (h) {
        // One-shot is guest registration state, never the lifetime of this
        // process's native/VM multiplexer. A host fault above must not consume it.
        if (g_guest_sig[sig].flags & KL_SA_RESETHAND) {
            void *expected = h;
            __atomic_compare_exchange_n(&g_guest_sig[sig].handler, &expected,
                (void *)SIG_DFL, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        }`);
  source=replace(source,'    if (uctx) {\n        ucontext_t *uc = (ucontext_t *)uctx;','    if (uctx && kl_sig_x18_repair_on()) {\n        ucontext_t *uc = (ucontext_t *)uctx;');
  const begin=source.indexOf('int klb_sigaction('),finish=source.indexOf('// ---------- uname ----------',begin);
  if(begin<0||finish<0)throw Error('Upstream sigaction unit changed');
  let action=source.slice(begin,finish);
  action=replace(action,'    struct sigaction d, o;','    struct sigaction d, o;\n    if (sig >= 32 && sig <= 64) return klj_executable_signal_action(sig,in,old);\n    int host_sig = klj_signal_to_host(sig);\n    if (host_sig <= 0) { errno = EINVAL; return -1; }\n    void *previous_guest = g_guest_sig[host_sig].handler;\n    void *previous_restorer = g_guest_sig[host_sig].restorer;\n    uint32_t previous_flags = g_guest_sig[host_sig].flags;\n    int previous_native = g_guest_sig[host_sig].native_only;');
  action=replace(action,'    if (in) {',`    if (in) {
        struct sigaction original;
        if (sigaction(host_sig, NULL, &original)) return -1;
        if (original.sa_sigaction != kl_sig_x18_tramp) {
            g_guest_sig[host_sig].native_action = original;
            g_guest_sig[host_sig].native_saved = 1;
        }`);
  action=replace(action,'    if (in) {\n        struct sigaction original;', '    if (in || old) {\n        struct sigaction original;');
  action=replace(action,'            g_guest_sig[host_sig].native_saved = 1;\n        }','            g_guest_sig[host_sig].native_saved = 1;\n        }\n    }\n    if (in) {');
  action=action.replaceAll('klb_fatal_signal(sig)','klb_fatal_signal(host_sig)');
  action=replace(action,'        int is_guest = in->handler && in->handler != (void *)SIG_DFL &&\n                       in->handler != (void *)SIG_IGN;','        int is_guest = in->handler != (void *)kl_sig_native_chain;');
  action=replace(action,'        if (is_guest && !klb_fatal_signal(host_sig) && sig > 0 && sig < KL_NSIG &&\n            kl_sig_x18_repair_on()) {','        if (is_guest) {');
  action=action.replaceAll('g_guest_sig[sig]','g_guest_sig[host_sig]').replaceAll('(sigset_t)(in->mask & 0xFFFFFFFFu)','klj_signal_mask_to_host(in->mask)');
  action=replace(action,'            g_guest_sig[host_sig].handler = (void *)in->handler;','            g_guest_sig[host_sig].handler = (void *)in->handler;\n            g_guest_sig[host_sig].restorer = in->restorer;\n            g_guest_sig[host_sig].flags = in->flags;');
  action=replace(action,'            d.sa_handler = (void (*)(int))in->handler;','            d.sa_handler = g_guest_sig[host_sig].native_action.sa_handler;');
  action=replace(action,'        if (is_guest) {','        if (is_guest) {\n            g_guest_sig[host_sig].native_only = 0;');
  action=replace(action,'            d.sa_flags     = kl_sa_flags((uint32_t)in->flags) | SA_SIGINFO;',`            // A guest's one-shot flag cannot reset the native fault dispatcher.
            // Retain the host VM's guard-page stack and syscall restart contract.
            d.sa_flags = (kl_sa_flags((uint32_t)in->flags) & ~SA_RESETHAND) |
                (g_guest_sig[host_sig].native_action.sa_flags & (SA_ONSTACK | SA_RESTART)) | SA_SIGINFO;`);
  action=replace(action,'sigaction(sig, in ? &d : NULL, old ? &o : NULL)','sigaction(host_sig, in ? &d : NULL, old ? &o : NULL)');
  action=replace(action,'    int r = sigaction(host_sig, in ? &d : NULL, old ? &o : NULL);',`    if (in && d.sa_sigaction != kl_sig_x18_tramp &&
        atomic_load_explicit(&klj_executable_mux_active,memory_order_acquire) &&
        (host_sig == SIGILL || host_sig == SIGBUS || host_sig == SIGSEGV)) {
        g_guest_sig[host_sig].native_action = d;
        g_guest_sig[host_sig].native_saved = 1;
        g_guest_sig[host_sig].native_only = 1;
        g_guest_sig[host_sig].handler = (void *)in->handler;
        g_guest_sig[host_sig].flags = in->flags;
        g_guest_sig[host_sig].restorer = in->restorer;
        d.sa_sigaction = kl_sig_x18_tramp;
        d.sa_flags = (d.sa_flags & ~SA_RESETHAND) | SA_SIGINFO;
    }
    int r = sigaction(host_sig, in ? &d : NULL, old ? &o : NULL);`);
  action=replace(action,'            oh = g_guest_sig[host_sig].handler;      // unwrap our trampoline',`            oh = previous_native ? (void *)kl_sig_native_chain : previous_guest;
        else if (oh != (void *)SIG_DFL && oh != (void *)SIG_IGN)
            oh = (void *)kl_sig_native_chain;`);
  action=replace(action,'        old->mask    = o.sa_mask;','        old->mask    = klj_signal_mask_to_guest(o.sa_mask);');
  action=replace(action,'        old->handler = oh;','        old->handler = oh;\n        old->restorer = o.sa_sigaction == kl_sig_x18_tramp ? previous_restorer : NULL;');
  action=replace(action,'        old->flags   = (int32_t)kl_sa_flags_back(o.sa_flags);','        old->flags   = o.sa_sigaction == kl_sig_x18_tramp ? (int32_t)previous_flags : (int32_t)kl_sa_flags_back(o.sa_flags);');
  const enable=`
int klj_executable_signal_enable(void) {
    if (atomic_load_explicit(&klj_executable_mux_active,memory_order_acquire) == 2) return 0;
    pthread_mutex_lock(&klj_executable_mux_lock);
    if (atomic_load_explicit(&klj_executable_mux_active,memory_order_acquire) == 2) {
        pthread_mutex_unlock(&klj_executable_mux_lock);return 0;
    }
    int result=0, saved=errno;
    atomic_store_explicit(&klj_executable_mux_active,1,memory_order_release);
    const int signals[]={SIGILL,SIGBUS,SIGSEGV};
    for (unsigned index=0;index<sizeof signals/sizeof signals[0];index++) {
        int sig=signals[index]; struct sigaction original;
        if (sigaction(sig,NULL,&original)) { result=-1; saved=errno; break; }
        if (original.sa_sigaction == kl_sig_x18_tramp) continue;
        g_guest_sig[sig].native_action=original;
        g_guest_sig[sig].native_saved=1;g_guest_sig[sig].native_only=1;
        g_guest_sig[sig].handler=(void *)original.sa_handler;
        g_guest_sig[sig].flags=kl_sa_flags_back(original.sa_flags);
        struct sigaction wrapped=original;
        wrapped.sa_sigaction=kl_sig_x18_tramp;
        wrapped.sa_flags=(wrapped.sa_flags & ~SA_RESETHAND)|SA_SIGINFO;
        if (sigaction(sig,&wrapped,NULL)) { result=-1; saved=errno; break; }
    }
    if (!result) atomic_store_explicit(&klj_executable_mux_active,2,memory_order_release);
    pthread_mutex_unlock(&klj_executable_mux_lock);errno=saved;return result;
}
`;
  return header+source.slice(0,begin)+action+enable+source.slice(finish);
}

/** The original pthread and ppoll owners share exactly the same mask encoding. */
export function prepareSignalCallers({pthread,slink}){
  let text=fs.readFileSync(pthread,'utf8');
  text=replace(text,'px(pthread_kill(t, sig))','px(klj_signal_thread(t, sig))');
  const begin=text.indexOf('// Same names, different numbers, and the consequence is invisible:'),end=text.indexOf('// bionic takes the thread;',begin);
  if(begin<0||end<0)throw Error('Upstream pthread signal mask unit changed');
  text=text.slice(0,begin)+`int klb_pthread_sigmask(int how,const uint64_t *s,uint64_t *o) { return px(klj_signal_mask(how,s,o)); }
int klb_sigprocmask(int how,const uint64_t *s,uint64_t *o) { int result=klj_signal_mask(how,s,o); if(result){errno=result;return -1;}return 0; }
`+text.slice(end);
  fs.writeFileSync(pthread,header+text);
  text=fs.readFileSync(slink,'utf8');
  text=replace(text,'    pthread_sigmask(SIG_SETMASK, (const sigset_t *)sigmask, &saved);','    sigset_t requested = klj_signal_mask_to_host(*(const uint64_t *)sigmask);\n    pthread_sigmask(SIG_SETMASK, &requested, &saved);');
  fs.writeFileSync(slink,header+text);
}
