import fs from 'node:fs';import path from 'node:path';import {fileURLToPath} from 'node:url';
import {adaptSignalSource} from '../signal-sources.mjs';
import {adaptFileStatusSource} from '../file-status/source.mjs';
import {adaptCpuAffinity} from '../cpu-affinity-source.mjs';
const source=path.dirname(fileURLToPath(import.meta.url));
const replace=(s,a,b)=>{if(s.split(a).length!==2)throw Error('Clock source anchor changed: '+a);return s.replace(a,b);};
/** Run after the existing lifecycle source generator, in a fresh private build.
 * The caller retains its exact native closure and existing shim adapter. */
export function prepareClockSources(klepton,build,shimSource) {
let pthread=fs.readFileSync(path.join(build,'pthread-source.c'),'utf8');
pthread=replace(pthread,'int klb_pthread_cond_init(void *c, const void *a)  { (void)a; cnd(c); return 0; }\nint klb_pthread_cond_destroy(void *p) { (void)p; return 0; }',fs.readFileSync(path.join(source,'cond-clock.inc'),'utf8'));
const start=pthread.indexOf('int klb_pthread_cond_timedwait(void *c,'),end=pthread.indexOf('// ---------- rwlock ----------',start);
if(start<0||end<0)throw Error('Missing condition wait region');
pthread=pthread.slice(0,start)+fs.readFileSync(path.join(source,'cond-wait.inc'),'utf8')+'\n'+pthread.slice(end);
for(const signature of ['int klb_pthread_cond_signal(void *c) {','int klb_pthread_cond_broadcast(void *c) {','int klb_pthread_cond_wait(void *c, void *m) {'])pthread=replace(pthread,signature,signature+'\n    if (!klj_cond_valid(c)) return EINVAL;\n    if (atomic_load((_Atomic(uint32_t) *)c) & 1) return px(ENOTSUP);');
fs.writeFileSync(path.join(build,'pthread-source.c'),pthread);
let libc=fs.readFileSync(path.join(klepton,'runtime/libc/kl_libc.c'),'utf8');
const cs=libc.indexOf('static int kl_clock_to_darwin(int clk)'),ce=libc.indexOf('// bionic struct timeval',cs);
if(cs<0||ce<0)throw Error('Missing clock region');
libc=libc.slice(0,cs)+`int klb_clock_getres(int clock, struct timespec *out) {
    return klj_clock_resolution(clock,out);
}
int klb_clock_gettime(int clock, struct timespec *out) { return klj_clock_read(clock, out); }
\n`+libc.slice(ce);
// Linux sysinfo uptime, unlike CLOCK_MONOTONIC, includes suspend.
libc=replace(libc,'if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) si->uptime = ts.tv_sec;','if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) == 0) si->uptime = ts.tv_sec;');
const bucketsStart=libc.indexOf('#define KL_FUTEX_BUCKETS '),bucketsEnd=libc.indexOf('static void ts_normalise(',bucketsStart);
if(bucketsStart<0||bucketsEnd<0)throw Error('Missing futex bucket owner');
libc=libc.slice(0,bucketsStart)+libc.slice(bucketsEnd);
const futexStart=libc.indexOf('// The tail three arguments are overloaded by op,'),futexEnd=libc.indexOf('// ...and WHO asked.',futexStart);
if(futexStart<0||futexEnd<0)throw Error('Missing futex syscall boundary');
const traceStart=libc.indexOf('static long kl_futex(',futexStart),traceEnd=libc.indexOf('// The read-modify-write half of FUTEX_WAKE_OP',traceStart);
if(traceStart<0||traceEnd<0)throw Error('Missing futex diagnostic wrapper');
let trace=libc.slice(traceStart,traceEnd).replace('    static pthread_once_t once = PTHREAD_ONCE_INIT;\n    pthread_once(&once, futex_init);\n','').replaceAll('kl_futex_impl(','klj_futex(');
libc=libc.slice(0,futexStart)+'#include '+JSON.stringify(path.join(source,'futex.c'))+'\n'+trace+libc.slice(futexEnd);
fs.writeFileSync(path.join(build,'libc-source.c'),adaptSignalSource(adaptFileStatusSource(adaptCpuAffinity(libc))));
let shim=fs.readFileSync(shimSource,'utf8');
shim=replace(shim,'static void *klj_resolve_native_import(const char *name) {','static void *klj_resolve_native_import(const char *name);\nstatic void *klj_clock_base_lookup(const char *name) {');
// Keep quoted local includes anchored to the caller's existing owned source.
shim=shim.replace(/^#include \"([^\"]+)\"$/gm,(_line,name)=>'#include '+JSON.stringify(path.resolve(path.dirname(shimSource),name)));
const symbols=['pthread_condattr_getclock','pthread_condattr_setpshared','pthread_condattr_getpshared','pthread_cond_timedwait_monotonic_np'];
shim+='\n#include '+JSON.stringify(path.join(source,'interval-timer.h'))+'\n#include '+JSON.stringify(path.join(source,'clock-sleep.h'))+'\n#include '+JSON.stringify(path.join(source,'../signal-stack.h'))+'\n'+symbols.map(n=>'extern void klb_'+n+'(void);').join('\n')+'\nstatic void *klj_resolve_native_import(const char *name) {\n'+symbols.map(n=>'    if (!strcmp(name,"'+n+'")) return (void *)klb_'+n+';').join('\n')+'\n    if (!strcmp(name,"getitimer")) return (void *)klj_getitimer;\n    if (!strcmp(name,"setitimer")) return (void *)klj_setitimer;\n    if (!strcmp(name,"clock_nanosleep")) return (void *)klj_domain_clock_nanosleep;\n    if (!strcmp(name,"sigaltstack")) return (void *)klj_sigaltstack;\n    return klj_clock_base_lookup(name);\n}\n';
shim='#include <stddef.h>\nextern int klj_sched_getaffinity_kernel(int,size_t,void *);\n'+shim;
shim=replace(shim,'    return klj_clock_base_lookup(name);','    if (!strcmp(name,"__sched_getaffinity")) return (void *)klj_sched_getaffinity_kernel;\n    return klj_clock_base_lookup(name);');
fs.writeFileSync(path.join(build,'shim-clock.c'),shim);
return {libc:path.join(build,'libc-source.c'),shim:path.join(build,'shim-clock.c'),bridge:path.join(source,'clock-bridge.c'),include:path.join(source,'clock-domain.h'),pthread:path.join(build,'pthread-source.c')};
}
