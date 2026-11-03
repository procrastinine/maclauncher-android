import {addLicense} from '../upstream/licenses.mjs';
import {prepareSourceTree,copyChanged,writeChanged} from '../upstream/source-files.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {exportSources} from '../binder-rpc/build.mjs';
import {readModuleList} from '../upstream/blueprint.mjs';
import {compileHostLibrary} from '../host-compile.mjs';

const root=import.meta.dirname;
const suppliers=JSON.parse(fs.readFileSync(path.join(root,'../keystore/sources.json')));
const revision=suppliers.find(source=>source.name==='java-framework').revision;
const unit='apex/jobscheduler/service/jni/com_android_server_alarm_AlarmManagerService.cpp';
const replace=(text,from,to)=>{if(text.split(from).length!==2)throw Error('Original alarm native boundary changed: '+from);return text.replace(from,to);};

/** Complete original alarm JNI module, sharing the runtime's descriptor owner. */
export function buildAlarm({frameworkSource,binder,pollable,processLibrary,output,ninja=process.env.NINJA||'ninja',python=process.env.PYTHON}) {
  if(!python)throw Error('Alarm build requires the configured Python executable');
  output=path.resolve(output);
  const source=path.join(output,'source');
  const module='apex/jobscheduler/service/jni';
  const units=prepareSourceTree(source,source=>{
    exportSources(frameworkSource,revision,[module,'NOTICE'],source);
    const files=readModuleList(path.join(source,module),'libalarm_jni','srcs');
    const file=path.join(source,unit);let text=fs.readFileSync(file,'utf8');
    // These Linux/platform utility headers have no used declarations in this
    // implementation. JNI registration and Android logging use the shared owner.
    for(const header of ['android-base/file.h','android-base/unique_fd.h','utils/String8.h','utils/misc.h','linux/ioctl.h','linux/rtc.h'])
      text=replace(text,'#include <'+header+'>','');
    text=replace(text,'namespace android {','#include "Portability.h"\n\nnamespace android {');
    text=text.replaceAll('clockid_t','int');
    text=replace(text,'return result >= 0 ? 0 : errno;','return result >= 0 ? 0 : kl_errno_to_linux(errno);');
    fs.writeFileSync(file,text);
    const onload=path.join(source,module,'onload.cpp');
    fs.writeFileSync(onload,replace(fs.readFileSync(onload,'utf8'),'#include "utils/misc.h"',''));
    return files;
  });
  const files=units.map(file=>path.join(source,module,file));
  const helper=path.join(binder.output,'source/nativehelper');
  const includes=[root,pollable.include,path.join(pollable.source,'src'),path.join(helper,'include'),path.join(helper,'include_jni'),path.join(helper,'include_platform_header_only'),path.join(helper,'header_only_include'),path.join(binder.output,'source/native/libs/binder/liblog_stub/include'),path.join(binder.output,'source/core/libutils/include')];
  const name='libalarm_jni.dylib',library=path.join(output,name),publication=path.join(output,'publication',name);
  fs.mkdirSync(path.dirname(publication),{recursive:true});
  compileHostLibrary({name,inputs:files,
    flags:['-std=c++20','-arch','arm64','-mmacosx-version-min=13.0','-O2','-fPIC','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-writable-strings','-DEPOLL_SHIM_DISABLE_WRAPPER_MACROS=1','-DCOMPAT_ENABLE_ITIMERSPEC',...includes.map(file=>'-I'+file),'-include',path.join(root,'../binder-rpc/hotspot-jni.h')],
    extra:[binder.library,processLibrary],directory:path.join(output,'objects'),output:publication,cwd:output,ninja,python,compiler:'clang++',pathRoots:[[path.dirname(output),'/build']]});
  for(const name of ['libandroid-binder-rpc.dylib','libklepton-jvm.dylib'])execFileSync('install_name_tool',['-change','@rpath/'+name,'@loader_path/'+name,publication],{stdio:'inherit'});
  execFileSync('codesign',['--force','--sign','-',publication],{stdio:'inherit'});
  copyChanged(publication,library);
  const licenses=path.join(output,'licenses');
  addLicense(path.dirname(licenses),'alarm',path.join(source,'NOTICE'));
  return {library,source,licenses};
}
