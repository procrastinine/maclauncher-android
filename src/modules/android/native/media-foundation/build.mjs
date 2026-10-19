import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {prepareExtractorBufferSource} from '../media-extractor-source.mjs';
import {exportSources,publisherList} from '../binder-rpc/build.mjs';
import {prepareVolumeShaper} from './volume-shaper-sources.mjs';
import {prepareSourceTree,copyChanged,synchronizeSourceTree} from '../upstream/source-files.mjs';
import {compileHostObject,compileHostLibrary} from '../host-compile.mjs';
import {buildLibvpx} from './libvpx-build.mjs';
const root=import.meta.dirname;
const pins=JSON.parse(fs.readFileSync(path.join(root,'../source-pins.json')));
/** Complete upstream format/message units share the existing native Binder and
 * libutils owner. Only the Darwin selection of original Parcel bodies differs. */
function prepareMediaFoundation({avSource,frameworkSource,aidl,klepton,binder,output,buildRoot}){
  const source=path.join(output,'source');
  // Consume complete publisher modules. Updating their pinned Blueprint source
  // lists automatically includes newly added implementation units.
  exportSources(avSource,pins.aospAv,['media/module/foundation','media/module/esds','media/libstagefright/include','media/ndk','media/libmedia/NdkMediaFormatPriv.cpp','include/media','media/ndk/include/media','media/ndk/NOTICE'],source);
  const modules=[{path:'media/module/foundation',name:'libstagefright_foundation_defaults'},{path:'media/module/esds',name:'libstagefright_esds'},{path:'media/ndk',name:'libmediandk_format'}];
  const units=['media/libmedia/NdkMediaFormatPriv.cpp',...modules.flatMap(module=>publisherList(path.join(source,module.path),module.name,'srcs').map(file=>module.path+'/'+file))];
  prepareExtractorBufferSource(klepton,source);
  const shaper=prepareVolumeShaper({avSource,frameworkSource,pins,source,output,aidl});
  const codecHeader=path.join(source,'media/ndk/include/media/NdkMediaCodec.h');
  const codecSource=fs.readFileSync(codecHeader,'utf8');
  fs.writeFileSync(codecHeader,codecSource.replace('#include <android/api-level.h>','#if defined(__ANDROID__)\n#include <android/api-level.h>\n#endif'));
  const guard='#if defined(__ANDROID__) && !defined(__ANDROID_VNDK__) && !defined(__ANDROID_APEX__)';
  for(const name of ['AMessage','AString']){
    const file=path.join(source,'media/module/foundation',name+'.cpp'),text=fs.readFileSync(file,'utf8');
    if(text.split(guard).length!==3)throw Error('AOSP media Parcel selection changed: '+name);
    fs.writeFileSync(file,text.replaceAll(guard,'#if !defined(__ANDROID_VNDK__) && !defined(__ANDROID_APEX__)'));
  }
  const includes=[source,buildRoot,shaper.include,shaper.jni,path.join(source,'media/module/esds/include'),path.join(source,'media/libstagefright/include'),path.join(source,'media/ndk/include/media'),path.join(source,'include'),path.join(source,'media/ndk/include'),path.join(source,'media/module/foundation/include'),path.join(source,'media/module/foundation/include/media/stagefright/foundation'),
    ...['jni','source/nativehelper/header_only_include','source/native/libs/binder/include','source/native/libs/binder/ndk/include_cpp','source/native/headers/media_plugin','source/native/libs/binder/liblog_stub/include','source/core/libutils/binder/include','source/core/libutils/include','source/core/libcutils/include','source/core/libsystem/include','source/libbase/include','source/nativehelper/include_jni'].map(file=>path.join(binder.output,file)),path.join(root,'../binder-rpc/include'),path.join(root,'..')];
  const flags=['-std=c++23','-arch','arm64','-mmacosx-version-min=13.0','-O2','-fPIC','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-multichar','-Wno-deprecated-declarations','-Wno-nontrivial-memcall','-Wno-sign-compare','-Wno-deprecated-enum-enum-conversion',
    '-Doff64_t=off_t','-DBINDER_NO_LIBBASE','-DBINDER_WITH_KERNEL_IPC','-DANDROID_UTILS_CALLSTACK_ENABLED=0','-DEXPORT=__attribute__((visibility("default")))',...includes.map(file=>'-I'+file)];
  const notices=path.join(output,'notices');fs.mkdirSync(notices,{recursive:true});fs.copyFileSync(path.join(source,'media/module/foundation/NOTICE'),path.join(notices,'NOTICE'));
  fs.copyFileSync(path.join(source,'media/ndk/NOTICE'),path.join(notices,'NDK-NOTICE'));
  fs.writeFileSync(path.join(notices,'source.json'),JSON.stringify({project:'Android media framework',url:'https://android.googlesource.com/platform/frameworks/av',revision:pins.aospAv,license:'Apache-2.0; original NDK header notices retained',modules,units,volumeShaper:{headers:['include/media/VolumeShaper.h','include/media/Interpolator.h'],serializers:shaper.units.map(file=>path.relative(output,file)),jni:{project:'platform/frameworks/base',revision:pins.aospFramework,path:'media/jni/android_media_VolumeShaper.h'}}},null,2)+'\n');
  return {source,flags,inputs:[...units.map(file=>path.join(source,file)),...shaper.units,...['MediaRuntime.cpp','MediaExtractorJni.cpp','MediaExtractorNdk.cpp','MediaExtractorMetadata.cpp','MediaCodecOwner.cpp','VideoDecoder.cpp','VpxVideoDecoder.cpp','MediaCodecCatalog.cpp','MediaCodecListJni.cpp','MediaCodecNdk.cpp','MediaCodecJni.cpp','MediaCodecStreams.cpp','AudioShaper.cpp'].map(file=>path.join(root,file))]};
}

/** The publisher media ABI retains the same Binder/format/codec owners.
 * Stable source publication and shared Clang planning preserve incremental work. */
export function buildMediaFoundation({avSource,frameworkSource,aidl,klepton,binder,output,processLibrary,libvpxSource,ninja=process.env.NINJA||'ninja',python=process.env.PYTHON}){
  if(!python)throw Error('Media build requires the configured Python executable');
  output=path.resolve(output);
  const vpx=buildLibvpx({source:libvpxSource,output:path.join(output,'libvpx')});
  const prepared=path.join(output,'prepared');
  const plan=prepareSourceTree(prepared,temporary=>{
    const plan=prepareMediaFoundation({avSource,frameworkSource,aidl,klepton,binder,output:temporary,buildRoot:path.dirname(output)});
    const relocate=value=>value.replaceAll(temporary+path.sep,prepared+path.sep);
    return {...plan,source:relocate(plan.source),flags:plan.flags.map(relocate),inputs:plan.inputs.map(relocate)};
  });
  plan.flags.push('-I'+vpx.include);
  const directory=path.join(output,'objects'),pathRoots=[[path.dirname(output),'/build'],[klepton,'/src/klepton']];
  const owner=compileHostObject({name:'MediaExtractorOwner.o',inputs:[path.join(root,'MediaExtractorOwner.m')],
    flags:[...plan.flags.filter(value=>!value.startsWith('-std=')),'-std=gnu11','-fobjc-arc','-fblocks'],directory,cwd:output,ninja,python,compiler:'clang',pathRoots});
  const name='libandroid-media.dylib',library=path.join(output,name),publication=path.join(output,'publication',name);
  fs.mkdirSync(path.dirname(publication),{recursive:true});
  compileHostLibrary({name,inputs:[...plan.inputs,owner.output],flags:plan.flags,
    extra:['-framework','AVFoundation','-framework','AudioToolbox','-framework','VideoToolbox','-framework','CoreVideo','-framework','CoreMedia','-framework','Foundation',binder.library,processLibrary,vpx.library],
    directory,output:publication,cwd:output,ninja,python,compiler:'clang++',pathRoots});
  for(const dependency of ['libandroid-binder-rpc.dylib','libklepton-jvm.dylib'])execFileSync('install_name_tool',['-change','@rpath/'+dependency,'@loader_path/'+dependency,publication],{stdio:'inherit'});
  execFileSync('codesign',['--force','--sign','-',publication],{stdio:'inherit'});
  copyChanged(publication,library);
  synchronizeSourceTree(path.join(prepared,'notices'),path.join(output,'notices'));
  synchronizeSourceTree(vpx.notices,path.join(output,'notices','libvpx'));
  return {library,source:plan.source};
}
