import {includeDependencies} from '../upstream/source-includes.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {exportSources} from '../binder-rpc/build.mjs';
import {prepareSourceTree,copyChanged} from '../upstream/source-files.mjs';
import {compileHostLibrary} from '../host-compile.mjs';
const root=import.meta.dirname;
const pins=JSON.parse(fs.readFileSync(path.join(root,'../source-pins.json')));
function replace(text,before,after){if(text.split(before).length!==2)throw Error('Original libcore integration boundary changed: '+before);return text.replace(before,after);}

/** Both original Memory implementation units, including their full native tables. */
export function buildLibcore({libcoreSource,artSource,binder,output,ninja=process.env.NINJA||'ninja',python=process.env.PYTHON}) {
  if(!python)throw Error('Libcore build requires the configured Python executable');
  output=path.resolve(output);
  const source=path.join(output,'source');
  prepareSourceTree(source,source=>{
    const units=['libcore_io_Memory.cpp','ScopedBytes.h','JniConstants.h'];
    exportSources(libcoreSource,pins.aospLibcore,[...units.map(file=>'luni/src/main/native/'+file),'NOTICE'],path.join(source,'libcore'));
    exportSources(artSource,pins.aospArt,['runtime/native/libcore_io_Memory.cc','NOTICE'],path.join(source,'art'));
    const libcore=path.join(source,'libcore/luni/src/main/native');
    let text=fs.readFileSync(path.join(libcore,'libcore_io_Memory.cpp'),'utf8');
    text=replace(text,'#include "Portability.h"','#include "MemoryPortability.h"');
    fs.writeFileSync(path.join(libcore,'libcore_io_Memory.cpp'),text);
    const scoped=path.join(libcore,'ScopedBytes.h');
    fs.writeFileSync(scoped,replace(fs.readFileSync(scoped,'utf8'),'jniThrowNullPointerException(mEnv);','jniThrowNullPointerException(mEnv, nullptr);'));
    const art=path.join(source,'art/runtime/native/libcore_io_Memory.cc');
    text=fs.readFileSync(art,'utf8');
    for(const include of ['libcore_io_Memory.h','jni/jni_internal.h','native_util.h','scoped_fast_native_object_access-inl.h'])
      text=replace(text,'#include "'+include+'"','');
    text=replace(text,'namespace art HIDDEN {','namespace art {');
    text=replace(text,'REGISTER_NATIVE_METHODS("libcore/io/Memory");','jniRegisterNativeMethods(env, "libcore/io/Memory", gMethods, NELEM(gMethods));');
    fs.writeFileSync(art,includeDependencies(text,'#include <nativehelper/JNIHelp.h>'));
  });
  const libcore=path.join(source,'libcore/luni/src/main/native');
  const art=path.join(source,'art/runtime/native/libcore_io_Memory.cc');
  const helper=path.join(binder.output,'source/nativehelper');
  const includes=[root,libcore,path.join(helper,'include'),path.join(helper,'include_jni'),path.join(helper,'include_platform_header_only'),path.join(helper,'header_only_include'),path.join(binder.output,'source/native/libs/binder/liblog_stub/include')];
  const name='libandroid-libcore.dylib',library=path.join(output,name),publication=path.join(output,'publication',name);
  fs.mkdirSync(path.dirname(publication),{recursive:true});
  compileHostLibrary({name,inputs:[path.join(libcore,'libcore_io_Memory.cpp'),art,path.join(root,'Runtime.cpp')],
    flags:['-std=c++20','-arch','arm64','-mmacosx-version-min=13.0','-O2','-fPIC','-Wall','-Wextra','-Werror','-Wno-unused-parameter',...includes.map(file=>'-I'+file),'-include',path.join(root,'../binder-rpc/hotspot-jni.h')],
    extra:[binder.library],directory:path.join(output,'objects'),output:publication,cwd:output,ninja,python,compiler:'clang++',pathRoots:[[path.dirname(output),'/build']]});
  execFileSync('install_name_tool',['-change','@rpath/libandroid-binder-rpc.dylib','@loader_path/libandroid-binder-rpc.dylib',publication],{stdio:'inherit'});
  execFileSync('codesign',['--force','--sign','-',publication],{stdio:'inherit'});
  copyChanged(publication,library);
  const notices=path.join(output,'notices');fs.mkdirSync(notices,{recursive:true});
  for(const project of ['libcore','art'])copyChanged(path.join(source,project,'NOTICE'),path.join(notices,project+'.txt'));
  return {library,source};
}
