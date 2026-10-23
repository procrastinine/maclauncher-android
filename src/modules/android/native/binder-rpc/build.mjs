import {exportSources} from '../upstream/source-export.mjs';
import {includeDependencies} from '../upstream/source-includes.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {prepareBinderJni} from './jni-sources.mjs';
import {prepareRpcSessionOwnership} from './rpc-sources.mjs';
import {parseBlueprint,readModuleList} from '../upstream/blueprint.mjs';
import {extendBinder} from './ndk-build.mjs';
import {useSharedFoundation} from './foundation-sources.mjs';
import {prepareNativeTrace} from './tracing-sources.mjs';
import {writeChanged,copyChanged,prepareSourceTree,synchronizeSourceTree} from '../upstream/source-files.mjs';
import {compileHostLibrary,compileHostObject} from '../host-compile.mjs';
import {buildAshmemCodec} from './ashmem-codec.mjs';
import {prepareBinderLifetime} from './lifetime-sources.mjs';

const root=path.dirname(fileURLToPath(import.meta.url));
const pins=JSON.parse(fs.readFileSync(path.join(root,'../source-pins.json')));
const run=(command,args,options={})=>execFileSync(command,args,{stdio:'inherit',...options});

export {exportSources} from '../upstream/source-export.mjs';

export function publisherList(directory,module,property,options={}){
  return readModuleList(directory,module,property,options);
}

export function prepareDarwinUtils(source){
  // Original libutils predates Darwin POSIX clocks and maps every domain to
  // wall time. Select the existing shared native clock domains, retaining the
  // upstream message deadlines and timeout arithmetic.
  const timers=path.join(source,'core/libutils/Timers.cpp');
  if(fs.existsSync(timers)){
    const text=fs.readFileSync(timers,'utf8'),start=text.indexOf('#else\nnsecs_t systemTime(int clock) {'),end=text.indexOf('\n#endif',start);
    if(start<0||end<start)throw Error('AOSP libutils clock boundary changed');
    fs.writeFileSync(timers,includeDependencies(text.slice(0,start)+`#else
nsecs_t systemTime(int clock) {
    checkClockId(clock);
    static constexpr int clocks[] = {0, 1, 2, 3, 7};
    timespec value = {};
    klj_clock_read(clocks[clock], &value);
    return nsecs_t(value.tv_sec)*1000000000LL + value.tv_nsec;
}`+text.slice(end),'#include "'+path.join(root,'../native-clock/clock-domain.h')+'"'));
    const condition=path.join(source,'core/libutils/include/utils/Condition.h'),original=fs.readFileSync(condition,'utf8');
    const opening='inline status_t Condition::waitRelative(Mutex& mutex, nsecs_t reltime) {';
    const closing='    return -pthread_cond_timedwait(&mCond, &mutex.mMutex, &ts);\n}';
    if(original.split(opening).length!==2||original.split(closing).length!==2)throw Error('AOSP libutils condition boundary changed');
    fs.writeFileSync(condition,original.replace(opening,opening+`
#if defined(__APPLE__)
    if (reltime < 0) return -ETIMEDOUT;
    timespec relative = {static_cast<time_t>(reltime/1000000000LL), static_cast<long>(reltime%1000000000LL)};
    return -pthread_cond_timedwait_relative_np(&mCond, &mutex.mMutex, &relative);
#else`).replace(closing,'    return -pthread_cond_timedwait(&mCond, &mutex.mMutex, &ts);\n#endif\n}'));
  }
}

export function prepareBinderMemory({binder,output,sources}){
  // MediaBuffer/AMessage share the complete original Binder memory family;
  // their heaps use the same descriptor and read-only sealing owner as Parcel.
  // Include the parent's other portable units as well, rather than selecting
  // only the symbols used by media. Lazy service registration requires the
  // Android system servicemanager; installed services use the shared RPC broker.
  const family=publisherList(binder,'libbinder','srcs').filter(name=>name!=='LazyServiceRegistrar.cpp');
  const file=path.join(binder,'MemoryHeapBase.cpp'),text=fs.readFileSync(file,'utf8');
  if(text.split('#include <linux/memfd.h>').length!==2||text.split('namespace android {').length!==2)throw Error('AOSP Binder heap boundary changed');
  const adapted=path.join(output,'MemoryHeapBase-darwin.cpp');
  fs.writeFileSync(adapted,text.replace('#include <linux/memfd.h>','#ifdef __BIONIC__\n#include <linux/memfd.h>\n#endif')
    .replace('namespace android {','extern "C" int klj_parcel_close(int);\n#define close klj_parcel_close\n\nnamespace android {'));
  sources.push(...family.map(name=>name==='MemoryHeapBase.cpp'?adapted:path.join(binder,name)));
  const log=path.join(binder,'liblog_stub/include/log/log.h'),original=fs.readFileSync(log,'utf8');
  const end='} // extern "C"';
  if(original.split(end).length!==2)throw Error('AOSP Binder log boundary changed');
  fs.writeFileSync(log,original.replace(end,`int __android_log_error_write(int tag, const char* subTag, int32_t uid,
                              const char* data, uint32_t dataLen);
#define android_errorWriteWithInfoLog(tag, subTag, uid, data, dataLen) \\
    __android_log_error_write(tag, subTag, uid, data, dataLen)
${end}`));
}

/** These original units expose Android process state, permission decisions and
 * callbacks. Bind their POSIX identity calls once at the native unit boundary.
 * RPC socket credentials, connection thread keys and host child management
 * remain in their existing physical platform units. */
export function prepareBinderLogicalIdentity(sources){
  const units=new Set(['IPCThreadState.cpp','android_util_Binder.cpp','Binder-darwin.cpp',
    'PermissionCache.cpp','BpBinder.cpp','ProcessState.cpp']);
  for(const file of sources)if(units.has(path.basename(file)))
    fs.writeFileSync(file,includeDependencies(fs.readFileSync(file,'utf8'),'#include "android-identity.h"'));
}

function prepareBinderRpc({nativeSource,systemCore,output,jni,services,keymap}){
  output=path.resolve(output);
  const source=path.join(output,'source');
  exportSources(nativeSource,pins.aospNative,['libs/binder','headers/media_plugin','NOTICE'],path.join(source,'native'));
  exportSources(systemCore,pins.aospSystemCore,jni?['libutils','libcutils','libsystem','include']:['libutils/binder','libutils/include','libutils/NOTICE'],path.join(source,'core'));
  const binder=path.join(source,'native/libs/binder'),utils=path.join(source,'core/libutils/binder');
  const sources=[...publisherList(binder,'libbinder_common_defaults','srcs').map(file=>path.join(binder,file)),
    ...publisherList(utils,'libutils_binder_impl_defaults_nodeps','srcs').map(file=>path.join(utils,file))];
  const trigger=path.join(binder,'FdTrigger.cpp'),original=fs.readFileSync(trigger,'utf8');
  const old='{.fd = mRead.get(), .events = 0, .revents = 0}';
  if(original.split(old).length!==2)throw Error('AOSP Binder shutdown pipe interest changed');
  // Darwin poll only reports a pipe's EOF when read readiness is requested.
  // Preserve the original trigger, ownership, polling and shutdown algorithms.
  const adapted=path.join(output,'FdTrigger-darwin.cpp');
  fs.writeFileSync(adapted,original.replace(old,'{.fd = mRead.get(), .events = POLLIN, .revents = 0}'));
  sources[sources.indexOf(trigger)]=adapted;
  prepareDarwinUtils(source);
  prepareRpcSessionOwnership(binder);
  const flags=['-std=c++20','-arch','arm64','-mmacosx-version-min=13.0','-O2','-fPIC',
    ...publisherList(binder,'binder_sdk_defaults','cflags'),'-DBUILDING_LIBBINDER','-DANDROID_UTILS_CALLSTACK_ENABLED=0',
    ...[path.join(binder,'include'),path.join(binder,'liblog_stub/include'),binder,path.join(utils,'include'),
      path.join(source,'core/libutils/include'),path.join(root,'include')].map(directory=>'-I'+directory),
    '-include',path.join(root,'darwin.h')];
  let cSources=[];
  if(jni){
    ({cSources}=prepareBinderJni({root,pins,source,output,binder,flags,sources,inputs:{...jni,native:nativeSource,core:systemCore},exportSources}));
    prepareBinderMemory({binder,output,sources});
    // Keep the whole cutils property implementation, including property_list.
    // Its native calls share the existing guest/JVM property owner.
    const propertyHeader='libc/include/sys/system_properties.h';
    exportSources(jni.bionic,pins.aospBionic,[propertyHeader],path.join(source,'bionic'));
    const propertyInclude=path.join(output,'properties-include');
    fs.mkdirSync(path.join(propertyInclude,'sys'),{recursive:true});
    const propertyText=fs.readFileSync(path.join(source,'bionic',propertyHeader),'utf8');
    fs.writeFileSync(path.join(propertyInclude,'sys/system_properties.h'),propertyText
      .replace(/__BIONIC_AVAILABILITY_GUARD\(\d+\)/g,'1')
      .replace(/__INTRODUCED_IN\(\d+\)/g,''));
    flags.push('-I'+propertyInclude);
    sources.push(path.join(source,'core/libcutils/properties.cpp'),path.join(root,'properties.cpp'));
    sources.push(path.join(source,'core/libcutils/trace-host.cpp'));
    prepareNativeTrace({frameworkSource:jni.framework,nativeSource,pins,source,output,sources,flags,exportSources});
  }
  let os=path.join(root,'OS_darwin.cpp');
  if(services){
    extendBinder({binder,output,sources,flags,aidl:services.aidl,framework:jni.framework,keymap});
    prepareBinderLifetime({sources,binder,output});
    os=useSharedFoundation({root,output,sources,flags,foundation:services.foundation});
  }
  const identityLibrary=jni?.processLibrary;
  if(jni&&!identityLibrary)throw Error('Android Binder JNI requires the shared process identity owner');
  if(identityLibrary){flags.push('-DKLJ_BINDER_DESCRIPTOR_OWNER=1');prepareBinderLogicalIdentity(sources);}
  const notices=path.join(output,'notices');fs.mkdirSync(notices,{recursive:true});
  fs.copyFileSync(path.join(source,'native/NOTICE'),path.join(notices,'aosp-native.txt'));
  fs.copyFileSync(path.join(source,'core/libutils/NOTICE'),path.join(notices,'aosp-libutils.txt'));
  if(jni)for(const [directory,name]of[['framework','aosp-framework'],['nativehelper','aosp-libnativehelper'],['libbase','aosp-libbase'],['bionic/libc','aosp-bionic'],['core/libcutils','aosp-libcutils']])
    fs.copyFileSync(path.join(source,directory,'NOTICE'),path.join(notices,name+'.txt'));
  const compiled=new Set(sources.map(file=>path.basename(file).replace(/-darwin(?=\.cpp$)/,'')));
  const moduleNames=jni?['libbinder_common_defaults','libbinder_android_defaults','libbinder_kernel_defaults','libbinder']:['libbinder_common_defaults'];
  const definitions=parseBlueprint(fs.readFileSync(path.join(binder,'Android.bp'),'utf8'),{file:'libs/binder/Android.bp'}).modules;
  const modules=moduleNames.map(name=>({name,manifest:'libs/binder/Android.bp',definition:definitions.find(module=>module.properties.name===name).properties,sources:publisherList(binder,name,'srcs').map(file=>({path:file.startsWith(':')?file:'libs/binder/'+file,
    disposition:compiled.has(file)?'included':name==='libbinder_android_defaults'?'darwin-adapter':'system-service-dependency',
    ...(!compiled.has(file)?{reason:name==='libbinder_android_defaults'?'OS_darwin.cpp owns sockets, mappings, descriptor credentials and clocks on macOS.':'Requires the Android system servicemanager/AIDL daemon interface; package-scoped installed services currently use original Binder RPC and the shared service broker.'}:{})}))}));
  fs.writeFileSync(path.join(notices,'source.json'),JSON.stringify({project:'Android Binder and libutils',url:'https://android.googlesource.com/platform/frameworks/native',revision:pins.aospNative,modules,
    dependencies:[{project:'platform/system/core',revision:pins.aospSystemCore,module:'libutils_binder_impl_defaults_nodeps',sources:publisherList(utils,'libutils_binder_impl_defaults_nodeps','srcs').map(file=>'libutils/binder/'+file)},
      ...(jni?[{project:'platform/system/core',revision:pins.aospSystemCore,sources:['libutils/Threads.cpp','libutils/Timers.cpp','libutils/SystemClock.cpp','libcutils/native_handle.cpp','libcutils/properties.cpp']}]:[])],
    compiledSources:sources.map(file=>path.relative(output,file)),platformAdapters:['OS_darwin.cpp',...(jni?['shared-memory.cpp','properties.cpp','ParcelTransport.cpp','JniRuntime.cpp','ObjectParcel.cpp']:[])]},null,2)+'\n');
  return {flags,sources,cSources,os,output};
}

/** Build the complete upstream portable Binder SDK with Darwin OS adapters.
 * Preparation owns only its source tree; Clang/Ninja own compiler products. */
export function buildBinderRpc({nativeSource,systemCore,output,jni,services,ashmem,ninja=process.env.NINJA||'ninja',python=process.env.PYTHON}){
  if(!python)throw Error('Binder build requires the configured Python executable');
  output=path.resolve(output);
  const prepared=path.join(output,'prepared');
  const plan=prepareSourceTree(prepared,temporary=>{
    const plan=prepareBinderRpc({nativeSource,systemCore,output:temporary,jni,services,keymap:path.join(path.dirname(output),'keymap')});
    const relocate=value=>typeof value==='string'?value.replaceAll(temporary+path.sep,prepared+path.sep):value;
    const extension=path.join(temporary,'extension.json');
    if(fs.existsSync(extension)){
      const value=JSON.parse(fs.readFileSync(extension,'utf8'));
      writeChanged(extension,JSON.stringify({...value,sources:value.sources.map(relocate),flags:value.flags.map(relocate)},null,2));
    }
    return {...plan,output:prepared,sources:plan.sources.map(relocate),cSources:plan.cSources.map(relocate),flags:plan.flags.map(relocate),os:relocate(plan.os)};
  });
  const name='libandroid-binder-rpc.dylib',library=path.join(output,name),publication=path.join(output,'publication',name);
  fs.mkdirSync(path.dirname(publication),{recursive:true});
  const cObjects=plan.cSources.map(input=>compileHostObject({
    name:'sync-'+path.basename(input)+'.o',inputs:[input],
    flags:['-std=gnu11','-arch','arm64','-mmacosx-version-min=13.0','-O2','-fPIC',...plan.flags.filter(flag=>flag.startsWith('-I'))],
    directory:path.join(output,'objects'),cwd:output,ninja,python,compiler:'clang',pathRoots:[[path.dirname(output),'/build']]
  }).output);
  if(jni){if(!ashmem?.source)throw Error('Binder ashmem requires its pinned AIM source');cObjects.push(buildAshmemCodec({...ashmem,output:path.join(output,'ashmem-codec')}));}
  compileHostLibrary({name,inputs:[...plan.sources,plan.os,...cObjects],flags:plan.flags,
    extra:jni?.processLibrary?[jni.processLibrary]:[],directory:path.join(output,'objects'),output:publication,
    cwd:output,ninja,python,compiler:'clang++',pathRoots:[[path.dirname(output),'/build']]});
  if(jni?.processLibrary)run('install_name_tool',['-change','@rpath/libklepton-jvm.dylib','@loader_path/libklepton-jvm.dylib',publication]);
  run('codesign',['--force','--sign','-',publication]);
  copyChanged(publication,library);
  synchronizeSourceTree(path.join(prepared,'notices'),path.join(output,'notices'));
  if(jni)synchronizeSourceTree(path.join(output,'ashmem-codec/notices'),path.join(output,'notices/aim-ashmem'));
  return {library,flags:plan.flags,sources:plan.sources,output:prepared};
}

if(process.argv[1]&&path.resolve(process.argv[1])===fileURLToPath(import.meta.url)){
  const options={};
  for(let i=2;i<process.argv.length;i+=2){const key=process.argv[i];if(!['--native-source','--system-core','--output','--python','--ninja'].includes(key)||!process.argv[i+1])throw Error('Expected --native-source, --system-core, --output, --python and --ninja paths');options[key.slice(2)]=path.resolve(process.argv[i+1]);}
  const checkout=name=>path.resolve(root,'../../../../external',name);
  buildBinderRpc({nativeSource:options['native-source']||checkout('aosp-native'),systemCore:options['system-core']||checkout('aosp-system-core'),output:options.output||path.resolve('build/android-binder-rpc'),python:options.python,ninja:options.ninja});
}
