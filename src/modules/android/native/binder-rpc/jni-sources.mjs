import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {prepareRpcIdentity} from './rpc-sources.mjs';
import {prepareKeyMapParcel} from './keymap-parcel.mjs';
import {prepareMotionParcel} from './motion-parcel.mjs';
import {prepareCursorParcel} from './cursor-parcel.mjs';
import {prepareSyncFence} from './sync-fence-sources.mjs';

function replace(text,before,after){
  if(text.split(before).length!==2)throw Error('AOSP Binder JVM adaptation changed: '+before);
  return text.replace(before,after);
}

/** Keep the complete native Parcel/Binder implementations and their JNI tables.
 * Adapt only host ABI, the absent kernel transport, and actual JDK layouts. */
export function prepareBinderJni({root,pins,source,output,binder,flags,sources,inputs,exportSources}){
  const framework=path.join(source,'framework'),helper=path.join(source,'nativehelper'),base=path.join(source,'libbase'),bionic=path.join(source,'bionic');
  exportSources(inputs.framework,pins.aospFramework,[
    'core/jni/android_os_Parcel.cpp',
    'core/jni/android_os_Parcel.h',
    'core/jni/android_util_Binder.cpp',
    'core/jni/android_util_Binder.h',
    'core/jni/android_util_CharsetUtils.cpp',
    'core/jni/android_os_SharedMemory.cpp',
    'core/jni/android_util_MemoryIntArray.cpp',
    'core/jni/core_jni_helpers.h',
    'core/jni/jni_wrappers.h',
    'core/jni/include/android_runtime/AndroidRuntime.h'
  ],framework);
  exportSources(inputs.nativehelper,pins.aospLibnativehelper,[
    'include',
    'include_jni',
    'include_platform',
    'include_platform_header_only',
    'header_only_include'
  ],helper);
  exportSources(inputs.libbase,pins.aospLibbase,['include', 'strings.cpp', 'stringprintf.cpp', 'posix_strerror_r.cpp'],base);
  // Consume the Binder ABI's complete header dependency closure. Exporting the
  // whole Linux tree would fetch thousands of unrelated device headers.
  const kernelRoots=['libc/kernel/uapi','libc/kernel/uapi/asm-arm64','libc/kernel/android/uapi'];
  const available=new Set(execFileSync('git',['-C',inputs.bionic,'ls-tree','-r','--name-only',pins.aospBionic,...kernelRoots],{encoding:'utf8'}).trim().split('\n'));
  const seen=new Set();
  function header(name){
    const file=kernelRoots.map(directory=>directory+'/'+name).find(file=>available.has(file));
    if(!file||seen.has(file))return;seen.add(file);
    const text=execFileSync('git',['-C',inputs.bionic,'show',pins.aospBionic+':'+file],{encoding:'utf8'});
    const target=path.join(bionic,file);fs.mkdirSync(path.dirname(target),{recursive:true});fs.writeFileSync(target,text);
    for(const include of text.matchAll(/^\s*#\s*include\s*[<"]([^>"]+)[>"]/gm))header(include[1]);
  }
  header('linux/android/binder.h');
  header('linux/sched.h');
  header('linux/sync_file.h');
  header('linux/ashmem.h');
  const generated=path.join(output,'jni');fs.mkdirSync(generated,{recursive:true});
  const parcelHeader=path.join(binder,'include/binder/Parcel.h');
  fs.writeFileSync(parcelHeader,replace(fs.readFileSync(parcelHeader,'utf8'),'    friend class RpcState;','    friend class RpcState;\n    friend class HostParcelTransport;'));
  // Linux's ioctl names collide with Darwin macros. Namespace the complete
  // original Linux definitions and their users, retaining the actual ABI values.
  const ioctl=path.join(bionic,'libc/kernel/uapi/asm-generic/ioctl.h');
  const macros=[...fs.readFileSync(ioctl,'utf8').matchAll(/^#\s*define\s+(\w+)/gm)].map(match=>match[1]).filter(name=>name!=='_UAPI_ASM_GENERIC_IOCTL_H');
  const tokens=new RegExp('\\b('+macros.join('|')+')\\b','g');
  function namespaceHeaders(directory){for(const entry of fs.readdirSync(directory,{withFileTypes:true})){const file=path.join(directory,entry.name);if(entry.isDirectory())namespaceHeaders(file);else if(entry.name.endsWith('.h')){const original=fs.readFileSync(file,'utf8'),text=original.replace(tokens,name=>'KLJ_LINUX_'+name);if(text!==original)fs.writeFileSync(file,text);}}}
  namespaceHeaders(path.join(bionic,'libc/kernel'));
  for(const file of fs.readdirSync(binder).filter(name=>name.endsWith('.cpp'))){
    const sourceFile=path.join(binder,file),text=fs.readFileSync(sourceFile,'utf8');
    const adapted=text.replace(tokens,name=>'KLJ_LINUX_'+name);if(text!==adapted)fs.writeFileSync(sourceFile,adapted);
  }
  const descriptors=path.join(binder,'include/binder/unique_fd.h');
  fs.writeFileSync(descriptors,'extern "C" int klj_parcel_close(int);\n'+replace(fs.readFileSync(descriptors,'utf8'),'::close(fd_);','::klj_parcel_close(fd_);'));
  const core=path.join(framework,'core/jni');
  let helpers=fs.readFileSync(path.join(core,'core_jni_helpers.h'),'utf8');
  helpers=replace(helpers,'jvm->AttachCurrentThread(&env, nullptr)','jvm->AttachCurrentThreadAsDaemon(&env, nullptr)');
  fs.writeFileSync(path.join(generated,'core_jni_helpers.h'),helpers);
  const sharedMemory=path.join(generated,'android_os_SharedMemory.cpp');
  fs.writeFileSync(sharedMemory,'#include "hotspot-jni.h"\n'+fs.readFileSync(path.join(core,'android_os_SharedMemory.cpp'),'utf8'));
  const memoryIntArray=path.join(generated,'android_util_MemoryIntArray.cpp');
  fs.writeFileSync(memoryIntArray,'#include "hotspot-jni.h"\n#include <sys/mman.h>\nextern "C" void* klj_ashmem_mmap(void*,size_t,int,int,int,off_t);\n#define mmap klj_ashmem_mmap\n'+fs.readFileSync(path.join(core,'android_util_MemoryIntArray.cpp'),'utf8'));
  sources.push(path.join(root,'CharsetArray.cpp'),sharedMemory,memoryIntArray);
  for(const name of ['android_os_Parcel','android_util_Binder']){
    let text=fs.readFileSync(path.join(core,name+'.cpp'),'utf8');
    // Both translation units include the service-manager facade without using it.
    text=replace(text,'#include <binder/IServiceManager.h>','');
    text=text.replace(/\(void\*\)(android_os_\w+)/g,'(void*)KlJni<$1>::call');
    if(name==='android_util_Binder'){
      text='#include "ParcelTransport.h"\n'+text;
      text=replace(text,'#include <binderthreadstate/CallerUtils.h>','');
      text=replace(text,'return getCurrentServingCall() == BinderCallType::BINDER;','auto* state = IPCThreadState::selfOrNull(); return state && state->getServingStackPointer();');
      text=replace(text,'return vm->GetEnv((void **)&env, JNI_VERSION_1_4) >= 0 ? env : NULL;','return kljBinderEnvironment(vm);');
      // The published Java Binder now owns the extension field. Its setter
      // calls this renamed native boundary; the getter is complete Java code.
      text=replace(text,'    { "getExtension", "()Landroid/os/IBinder;", (void*)KlJni<android_os_Binder_getExtension>::call },\n','');
      text=replace(text,'{ "setExtension", "(Landroid/os/IBinder;)V",','{ "setExtensionNative", "(Landroid/os/IBinder;)V",');
      text=replace(text,'    status_t err = target->transact(code, *data, reply, flags);','    status_t err = HostParcelTransport::transact(target, code, *data, reply, flags);');
    }else{
      // Java payloads use one original Parcel object/reference representation.
      // Binding Parcel.obtain(remote) to an RPC wire session here would reject
      // Binder references from another provider before the transport can route
      // them. Bind and export once, at HostParcelTransport::transact, for both
      // generated AIDL and older plain Parcel.obtain callers. Keep upstream's
      // empty-payload format assertion; only the transport-selection hint moves.
      text=replace(text,'        parcel->markForBinder(nBinder);','        parcel->markForBinder(nullptr);');
      text=replace(text,'static const JNINativeMethod gParcelMethods[] = {','#include "parcel-buffer-jni.h"\nstatic const JNINativeMethod gParcelMethods[] = {\n    {"nativeMarshallArray", "(J[BII)I", (void*)kljParcelMarshallArray},\n    {"nativeMarshallBuffer", "(JLjava/nio/ByteBuffer;II)I", (void*)kljParcelMarshallBuffer},\n    {"nativeUnmarshallBuffer", "(JLjava/nio/ByteBuffer;II)V", (void*)kljParcelUnmarshallBuffer},');
    }
    const metadata=[...text.matchAll(/\b(\w+(?:\.\w+)+)\s*=\s*MakeGlobalRefOrDie\(/g)].map(match=>match[1]);
    const parcelNamespace=name==='android_os_Parcel';
    text+='\n'+(parcelNamespace?'namespace android {\n':'')+'void '+(parcelNamespace?'kljReleaseParcelJniMetadata':'kljReleaseBinderJniMetadata')+'(JNIEnv* env){\n'+metadata.map(field=>'    if('+field+'){env->DeleteGlobalRef('+field+');'+field+'=nullptr;}').join('\n')+'\n}\n'+(parcelNamespace?'}\n':'');
    const file=path.join(generated,name+'.cpp');fs.writeFileSync(file,'#include "hotspot-jni.h"\n'+text);sources.push(file);
  }
  const process=path.join(generated,'ProcessState.cpp');
  fs.writeFileSync(process,replace(fs.readFileSync(path.join(binder,'ProcessState.cpp'),'utf8'),'#include <binder/IServiceManager.h>',''));
  const parcel=path.join(generated,'Parcel.cpp');
  let parcelSource=fs.readFileSync(path.join(binder,'Parcel.cpp'),'utf8');
  // BaseBundle retains lazy payloads in fresh Parcels. A fresh destination has
  // no wire objects or format of its own: retain the source RPC session before
  // upstream appendFrom copies bytes and acquires its Binder/FD references.
  // Existing payloads and different live RPC sessions keep upstream validation.
  parcelSource=replace(parcelSource,'status_t Parcel::appendFrom(const Parcel* parcel, size_t offset, size_t len) {',
    `status_t Parcel::appendFrom(const Parcel* parcel, size_t offset, size_t len) {
    if (!mData && !mDataSize && !mDataPos && !isForRpc() && parcel->isForRpc()) {
        markForRpc(parcel->maybeRpcFields()->mSession);
    }`);
  // Plain Parcel.obtain() is still used by supplied older AIDL generators.
  // Store real RPC Binder owners locally with the existing reference machinery;
  // HostParcelTransport exports them over RPC before any process boundary.
  parcelSource=replace(parcelSource,`                if (proxy->isRpcBinder()) {
                    ALOGE("Sending a socket binder over kernel binder is prohibited");
                    return INVALID_OPERATION;
                }`,`                if (proxy->isRpcBinder()) {
                    obj.hdr.type = BINDER_TYPE_BINDER;
                    obj.flags = 0;
                    obj.binder = reinterpret_cast<uintptr_t>(binder->getWeakRefs());
                    obj.cookie = reinterpret_cast<uintptr_t>(binder.get());
                    status_t status = writeObject(obj, false);
                    return status == OK ? finishFlattenBinder(binder) : status;
                }`);
  fs.writeFileSync(parcel,'#include "hotspot-jni.h"\n'+parcelSource.replace(/\bclose\(/g,'klj_parcel_close('));
  sources[sources.indexOf(path.join(binder,'Parcel.cpp'))]=parcel;
  const threadState=path.join(generated,'IPCThreadState.cpp');
  fs.copyFileSync(path.join(binder,'IPCThreadState.cpp'),threadState);
  sources.push(process,threadState,...['Static.cpp','BufferedTextOutput.cpp'].map(name=>path.join(binder,name)),
    ...['Threads.cpp','Timers.cpp','SystemClock.cpp'].map(name=>path.join(source,'core/libutils',name)),
    path.join(source,'core/libcutils/native_handle.cpp'),...['strings.cpp','stringprintf.cpp','posix_strerror_r.cpp'].map(name=>path.join(base,name)),
    path.join(root,'shared-memory.cpp'),path.join(root,'JniRuntime.cpp'),path.join(root,'ObjectParcel.cpp'),path.join(root,'ParcelTransport.cpp'));
  prepareRpcIdentity({root,binder,generated,sources});
  // Supplied native input objects retain their original wire algorithms. The
  // publisher's Parcel helper/primitives are host stubs, so these exact native
  // serializers must link to the same real Parcel owner as Java Binder.
  const input={framework:inputs.framework,native:inputs.native,pins,generated};
  const motion=prepareMotionParcel(input);
  sources.push(prepareKeyMapParcel(input),motion.file);
  const cursor=prepareCursorParcel({framework:inputs.javaFramework,publisher:inputs.publisher,generated});
  sources.push(...cursor.sources);flags.push('-I'+cursor.include);
  flags.push('-I'+path.join(path.dirname(output),'keymap'),'-I'+motion.include);
  const cSources=prepareSyncFence({pins,source,output,inputs,exportSources,sources,flags,ioctlTokens:tokens});
  for(const flag of ['-DBINDER_DISABLE_NATIVE_HANDLE','-DBINDER_DISABLE_BLOB']){const index=flags.indexOf(flag);if(index>=0)flags.splice(index,1);}
  // C++23 standardizes <stdatomic.h> interoperability used by original cutils.
  flags[flags.indexOf('-std=c++20')]='-std=c++23';
  flags.push('-DBINDER_WITH_KERNEL_IPC',...[generated,root,path.dirname(root),core,path.join(core,'include'),path.join(base,'include'),path.join(source,'core/libcutils/include'),path.join(source,'core/libsystem/include'),path.join(source,'core/include'),
    path.join(bionic,'libc/kernel/uapi'),path.join(bionic,'libc/kernel/uapi/asm-arm64'),path.join(bionic,'libc/kernel/android/uapi'),
    ...['include','include_jni','include_platform','include_platform_header_only','header_only_include'].map(name=>path.join(helper,name))].map(directory=>'-I'+directory));
  return {cSources};
}
