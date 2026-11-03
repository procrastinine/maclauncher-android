import fs from 'node:fs';
import {nativePathFlags} from './upstream/native-paths.mjs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {exportSources} from './binder-rpc/build.mjs';
import {prepareOpenSlConfiguration} from './opensl-configuration.mjs';

const pins=JSON.parse(fs.readFileSync(new URL('./source-pins.json',import.meta.url)));

/** Original UUID data and complete pointer exports; audio interfaces keep their native owner. */
export function prepareOpenSlIds({wilhelmSource,build,jni=[]}){
  const directory=path.join(build,'opensl-ids'),source=path.join(directory,'source');
  const units=['src/OpenSLES_IID.cpp','src/sl_iid.cpp','src/MPH.h','src/libOpenSLES.map.txt',
    'src/android/AudioPlayer_to_android.cpp','src/android/AudioRecorder_to_android.cpp','src/android/android_defs.h'];
  exportSources(wilhelmSource,pins.aospWilhelm,['include',...units],source);
  prepareOpenSlConfiguration(source,build);
  const original=fs.readFileSync(path.join(source,'src/sl_iid.cpp'),'utf8');
  const names=[...original.matchAll(/const SLInterfaceID (SL_IID_\w+)\s*=/g)].map(match=>match[1]);
  if(!names.length)throw Error('OpenSL interface definitions are unavailable');
  // The original umbrella also includes Android media-server implementations.
  // This complete data unit only needs its published types and UUID table.
  const aliases=original.replace('#include "sles_allinclusive.h"','');
  const text='#define ANDROID 1\n#define LI_API\n#include <string.h>\n#include '+JSON.stringify(path.join(source,'src/OpenSLES_IID.cpp'))+'\n'+aliases+'\nextern "C" void *klj_opensl_iid_lookup(const char *name) {\n'+
    names.map(name=>'  if (strcmp(name,"'+name+'") == 0) return (void *)&'+name+';').join('\n')+'\n  return nullptr;\n}\n';
  const unit=path.join(directory,'ids.cpp'),object=path.join(directory,'ids.o');fs.writeFileSync(unit,text);
  execFileSync('clang++',[...nativePathFlags({source:wilhelmSource,build}),'-std=c++17','-arch','arm64','-mmacosx-version-min=13.0','-O2','-fPIC','-Wall','-Wextra','-Werror','-I'+path.join(source,'include'),...jni,'-c',unit,'-o',object],{stdio:'inherit'});
  fs.writeFileSync(path.join(build,'opensl-ids.h'),names.map(name=>'extern const struct SLInterfaceID_ *const '+name+';').join('\n')+'\nvoid *klj_opensl_iid_lookup(const char *name);\n');
  return {object,names};
}

export function openSlIdSource(source){
  const start=source.indexOf('#define IID(sym, tag)'),end=source.indexOf('// ---- vtable layouts, in spec order ----',start);
  if(start<0||end<0)throw Error('Pinned OpenSL interface declaration boundary changed');
  source=source.slice(0,start)+'#include "opensl-ids.h"\n\n'+source.slice(end);
  source=source.replace(/^\s*\{"SL_IID_\w+",[^\n]+\n/gm,'');
  const entry='void *kl_opensl_sym(const char *name) {\n    if (!name) return NULL;';
  if(source.split(entry).length!==2)throw Error('Pinned OpenSL symbol boundary changed');
  return source.replace(entry,entry+'\n    void *iid = klj_opensl_iid_lookup(name);\n    if (iid) return iid;');
}
