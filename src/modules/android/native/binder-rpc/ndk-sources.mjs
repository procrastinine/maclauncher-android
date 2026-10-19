import fs from 'node:fs';import path from 'node:path';
export function prepareNdkSources({work,binder}) {
const put=(p,t)=>{fs.mkdirSync(path.dirname(work+'/'+p),{recursive:true});fs.writeFileSync(work+'/'+p,t)};
// The whole Binder/NDK facade uses the existing host logging implementation.
// Preserve Android severity before Darwin syslog macros can expand it.
let log=fs.readFileSync(binder+'/liblog_stub/include/log/log.h','utf8');
const a=log.indexOf('#define ALOG(priority,'),b=log.indexOf('#define ALOGV(',a);if(a<0||b<a)throw Error('Original log macro changed');
let macro=log.slice(a,b).replace('#define ALOG(', '#define KLJ_ALOG_ENUM(').replaceAll('ANDROID_##priority','priority').replace('IF_ALOG(priority, tag)', 'if (__android_log_stub_is_loggable(priority) && __ANDROID_LOG_STUB_IS_PRINT_PRESENT)');
log=log.slice(0,a)+macro+'#define ALOG(priority, tag, ...) KLJ_ALOG_ENUM(ANDROID_##priority, tag, __VA_ARGS__)\n'+log.slice(b);
log=log.replace('ALOG(priority, tag, #cond', 'KLJ_ALOG_ENUM(ANDROID_##priority, tag, #cond');put('include/log/log.h',log);
for(const [input,output]of[['ndk/include_ndk/android/binder_status.h','include/android/binder_status.h'],['include/binder/Delegate.h','include/binder/Delegate.h']]){let s=fs.readFileSync(binder+'/'+input,'utf8');s=s.replace('#define __assert(file, line, message)', '#ifdef __assert\n#undef __assert\n#endif\n#define __assert(file, line, message)');put(output,s);}
// These APIs describe host socket addresses. Use the actual Darwin layout,
// including sa_len, instead of placing Linux sockaddr bytes into native calls.
let rpc=fs.readFileSync(binder+'/ndk/binder_rpc.cpp','utf8').replace('#include <linux/vm_sockets.h>', '#include <sys/vsock.h>');put('binder_rpc-darwin.cpp',rpc);
let vm=fs.readFileSync(binder+'/vm_sockets.h','utf8').replace('#ifdef __BIONIC__','#if defined(__APPLE__)\n#include <sys/vsock.h>\n#elif defined(__BIONIC__)');put('include/vm_sockets.h',vm);
let bb=fs.readFileSync(binder+'/Binder.cpp','utf8');
const debugPid='pid_t BBinder::getDebugPid() {\n#ifdef __linux__';
if(bb.split(debugPid).length!==2)throw Error('Original Binder debug process identity changed');
bb=bb.replace(debugPid,'pid_t BBinder::getDebugPid() {\n#if defined(__linux__) || defined(__APPLE__)');
for(const marker of ['    int mPolicy = SCHED_NORMAL;', 'void BBinder::setMinSchedulerPolicy(int policy, int priority) {', '        if (getMinSchedulerPolicy() != SCHED_NORMAL) {']){
 const start=bb.indexOf('#ifdef __linux__\n'+marker);if(start<0)throw Error('Original Binder scheduler metadata changed');
 bb=bb.slice(0,start)+bb.slice(start).replace('#ifdef __linux__','#if defined(__linux__) || defined(__APPLE__)');
}
// Android Binder's policy metadata is an Android ABI value, not a Darwin
// pthread policy argument. The actual RPC worker remains with its existing owner.
bb=bb.replace('namespace android {','namespace android {\n#if defined(__APPLE__)\n#undef SCHED_NORMAL\n#undef SCHED_FIFO\n#undef SCHED_RR\n#define SCHED_NORMAL 0\n#define SCHED_FIFO 1\n#define SCHED_RR 2\n#endif');
put('Binder-darwin.cpp',bb);

put('BackendUnifiedServiceManager-darwin.cpp',fs.readFileSync(binder+'/BackendUnifiedServiceManager.cpp','utf8').replaceAll('base::unique_fd','binder::unique_fd'));

}
