import {includeDependencies} from '../upstream/source-includes.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {exportSources} from '../binder-rpc/build.mjs';
import {writeChanged} from '../upstream/source-files.mjs';

const root=import.meta.dirname;
const pins=JSON.parse(fs.readFileSync(path.join(root,'../source-pins.json')));

/** Retain the whole ActivityThread JNI unit; adapt only its allocator calls. */
export function prepareActivityThread({frameworkSource,bionicSource,build}){
  const directory=path.join(build,'activity-thread'),source=path.join(directory,'source');
  exportSources(frameworkSource,pins.aospFramework,['core/jni/android_app_ActivityThread.cpp'],source);
  exportSources(bionicSource,pins.aospBionic,['libc/include/malloc.h','libc/platform/bionic/malloc.h','libc/NOTICE'],path.join(source,'bionic'));
  const original=fs.readFileSync(path.join(source,'core/jni/android_app_ActivityThread.cpp'),'utf8');
  const mallocHeader=fs.readFileSync(path.join(source,'bionic/libc/include/malloc.h'),'utf8');
  const platformHeader=fs.readFileSync(path.join(source,'bionic/libc/platform/bionic/malloc.h'),'utf8');
  const purge=mallocHeader.match(/^#define M_PURGE (.+)$/m)?.[1];
  const profiling=platformHeader.match(/\bM_INIT_ZYGOTE_CHILD_PROFILING\s*=\s*(\d+)/)?.[1];
  if(!purge||!profiling)throw Error('Pinned allocator operation declarations changed');
  const methods=[...original.matchAll(/\{\s*"(\w+)"\s*,\s*"([^"]+)"\s*,\s*\(void\*\)\s*(\w+)\s*\}/g)];
  if(!methods.length)throw Error('Pinned ActivityThread JNI registration is absent');
  // These exports feed the ordinary JNI resolver and shared native registration
  // owner. Every entry in the original table is emitted, including optional ones.
  const exports=methods.map(([,name,descriptor,target])=>{
    if(descriptor!=='()V')throw Error('New ActivityThread native descriptor needs an export ABI: '+descriptor);
    return `extern "C" JNIEXPORT void JNICALL Java_android_app_ActivityThread_${name}(JNIEnv* env,jobject object){android::${target}(env,object);}`;
  }).join('\n');
  const includes=/^#include (?:<nativehelper\/JNIPlatformHelp\.h>|"core_jni_helpers\.h"|<bionic\/malloc\.h>)\n/gm;
  const unit=path.join(directory,'activity-thread.cpp');
  fs.mkdirSync(directory,{recursive:true});
  writeChanged(unit,includeDependencies(original.replace(includes,''),`#define M_PURGE ${purge}\n#define M_INIT_ZYGOTE_CHILD_PROFILING ${profiling}\n#include ${JSON.stringify(path.join(root,'allocator.h'))}`)+'\n'+exports+'\n');
  const notices=path.join(directory,'notices');fs.mkdirSync(notices,{recursive:true});
  fs.copyFileSync(path.join(source,'bionic/libc/NOTICE'),path.join(notices,'NOTICE'));
  fs.writeFileSync(path.join(notices,'source.json'),JSON.stringify({
    framework:{revision:pins.aospFramework,unit:'core/jni/android_app_ActivityThread.cpp'},
    bionic:{revision:pins.aospBionic,headers:['libc/include/malloc.h','libc/platform/bionic/malloc.h']},
    methods:methods.map(([,name,descriptor])=>({name,descriptor})),
    adaptation:'Darwin allocator pressure relief; Android zygote heap profiling has no host allocator backend.'
  },null,2)+'\n');
  return {unit,notices};
}
