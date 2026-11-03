import {addLicense} from '../upstream/licenses.mjs';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {fileURLToPath} from 'node:url';
import {execFileSync} from 'node:child_process';
import {exportSources} from '../binder-rpc/build.mjs';
import {nativePathFlags} from '../upstream/native-paths.mjs';
import {publisherFunctions} from './publisher-symbols.mjs';

const root=path.dirname(fileURLToPath(import.meta.url));
const framework=JSON.parse(fs.readFileSync(new URL('../framework-binary.json',import.meta.url)));
const pins=JSON.parse(fs.readFileSync(new URL('../source-pins.json',import.meta.url)));
const catalog=JSON.parse(fs.readFileSync(new URL('./sources.json',import.meta.url)));
const run=(command,args)=>execFileSync(command,args,{stdio:'inherit'});
const read=(command,args)=>execFileSync(command,args,{encoding:'utf8',maxBuffer:32<<20}).trim();
const units=['native/graphics/jni/bitmap.cpp','libs/hwui/apex/android_bitmap.cpp','native/android/asset_manager.cpp','native/android/configuration.cpp','native/graphics/jni/imagedecoder.cpp','native/graphics/jni/aassetstreamadaptor.cpp'];

export function prepareNdkGraphicsSources({frameworkSource,skiaSource,nativeSource,incfsSource,ziparchiveSource,output}) {
  const sources=Object.fromEntries(['framework','skia','native','incfs','ziparchive'].map(name=>[name,path.join(output,'source',name)]));
  const files=read('git',['-C',frameworkSource,'ls-tree','-r','--name-only',framework.declarationSource.revision,'libs/hwui','libs/androidfw/include','native/graphics','native/android','core/jni/include','NOTICE'])
    .split('\n').filter(file=>/\.(h|hpp|inc)$/.test(file)||units.includes(file)||file==='NOTICE');
  exportSources(frameworkSource,framework.declarationSource.revision,files,sources.framework);
  exportSources(skiaSource,catalog[0].revision,catalog[0].trees,sources.skia);
  exportSources(nativeSource,pins.aospNative,['include','libs/nativewindow/include','libs/arect/include'],sources.native);
  for(const [name,checkout]of [['incfs',incfsSource],['ziparchive',ziparchiveSource]]){
    const dependency=catalog.find(entry=>entry.name===name);
    exportSources(checkout,dependency.revision,dependency.trees,sources[name]);
  }
  return sources;
}

/** Original NDK graphics/resource families share the publisher's real objects.
 * Platform adaptations supply pixel lock views and owned physical file ranges. */
export function buildNdkGraphics({output,frameworkSource,skiaSource,nativeSource,incfsSource,ziparchiveSource,fmtSource,jdk,publisher,binder,loggingSource,processLibrary,sources}) {
  output=path.resolve(output);fs.mkdirSync(output,{recursive:true});
  if(crypto.createHash('sha256').update(fs.readFileSync(publisher)).digest('hex')!==framework.nativeRuntime.sha256)
    throw Error('NDK graphics requires the declared publisher image');
  sources??=prepareNdkGraphicsSources({frameworkSource,skiaSource,nativeSource,incfsSource,ziparchiveSource,output});
  const hwui=path.join(sources.framework,'libs/hwui');
  const native=path.join(binder.output,'source/native'),core=path.join(binder.output,'source/core');
  const logging=path.join(loggingSource,'liblog/include');
  const includes=[output,path.join(binder.output,'source/nativehelper/include_jni'),path.join(jdk,'include'),path.join(jdk,'include/darwin'),
    path.join(sources.skia,'mac'),path.join(sources.skia,'client_utils/android'),hwui,path.join(hwui,'jni'),path.join(hwui,'apex'),path.join(hwui,'apex/include'),
    path.join(sources.framework,'native/graphics/jni'),path.join(sources.framework,'native/android/include'),path.join(sources.framework,'core/jni/include'),path.join(sources.framework,'libs/androidfw/include'),
    path.join(sources.incfs,'incfs/util/include'),path.join(sources.incfs,'incfs/include'),path.join(sources.ziparchive,'include'),path.join(fmtSource,'include'),sources.skia,
    ...new Set(fs.globSync('include/**/*.h',{cwd:sources.skia}).map(file=>path.join(sources.skia,path.dirname(file)))),
    path.join(sources.native,'include'),path.join(sources.native,'libs/nativewindow/include'),path.join(sources.native,'libs/arect/include'),path.join(native,'include'),path.join(binder.output,'source/libbase/include'),path.join(core,'libcutils/include'),path.join(core,'libsystem/include'),path.join(core,'libutils/include'),
    path.join(core,'libutils/binder/include'),path.join(binder.output,'source/nativehelper/include'),path.join(binder.output,'source/nativehelper/include_platform'),
    path.join(binder.output,'source/nativehelper/header_only_include'),logging];
  const flags=['-std=c++20','-O2','-DNDEBUG','-fno-rtti','-arch','arm64','-mmacosx-version-min=13.0',
    ...includes.map(dir=>'-I'+dir),...nativePathFlags({build:output,roots:[[frameworkSource,'/src/framework'],[skiaSource,'/src/skia'],[nativeSource,'/src/native'],[binder.output,'/build/binder'],[jdk,'/src/jdk']]})];
  // Layoutlib's host Bitmap owns CPU pixels and cannot create Hardware storage.
  // Keep the NDK query callable and its normal null result for those bitmaps;
  // the complete hardware-buffer implementation remains owned by graphics.
  const original=fs.readFileSync(path.join(sources.framework,units[1]),'utf8');
  const hardware=/AHardwareBuffer\* ABitmap_getHardwareBuffer\(ABitmap\* bitmapHandle\) \{[\s\S]*?\n\}/g;
  if([...original.matchAll(hardware)].length!==1)throw Error('AOSP bitmap hardware boundary changed');
  const adapted=path.join(output,'android_bitmap.cpp');
  fs.writeFileSync(adapted,original.replace(hardware,'AHardwareBuffer* ABitmap_getHardwareBuffer(ABitmap*) { return nullptr; }')
    .replace('case kN32_SkColorType:','case kRGBA_8888_SkColorType:\n        case kBGRA_8888_SkColorType:')
    .replaceAll('kN32_SkColorType','kRGBA_8888_SkColorType'));
  const ndk=path.join(output,'ndk-bitmap.cpp');
  fs.writeFileSync(ndk,fs.readFileSync(path.join(sources.framework,units[0]),'utf8')
    .replace('int AndroidBitmap_lockPixels(', 'extern "C" int AndroidBitmap_lockPixels_original(')
    .replace('int AndroidBitmap_unlockPixels(', 'extern "C" int AndroidBitmap_unlockPixels_original('));
  const assets=path.join(output,'ndk-assets.cpp');
  const originalAssets=fs.readFileSync(path.join(sources.framework,units[2]),'utf8');
  const fromJava='    return (AAssetManager*) env->GetLongField(assetManager, gAssetManagerOffsets.mObject);';
  const open='    if (asset == nullptr) {\n        return nullptr;\n    }\n    return new AAsset(std::move(asset));';
  if(originalAssets.split(fromJava).length!==2||originalAssets.split(open).length!==2)throw Error('Original NDK asset boundary changed');
  fs.writeFileSync(assets,'#include '+JSON.stringify(path.join(root,'asset-files.h'))+'\n'+originalAssets
    .replace(fromJava,'    return NdkAssetManagerForJavaObject(env,assetManager);')
    .replace(open,'    if (!asset) asset=klj_asset_file(filename,amMode);\n    klj_asset_opened(filename,mode,asset?asset->getLength():-1,asset?0:errno);\n    if (!asset) return nullptr;\n    return new AAsset(std::move(asset));'));
  // Keep the original decoder and Asset stream owner. The host publisher uses
  // BGRA storage; the NDK buffer boundary retains its public RGBA layout.
  let decoder=fs.readFileSync(path.join(sources.framework,units[4]),'utf8');
  const decode='    const auto result = ResultToErrorCode(imageDecoder->decode(pixels, stride));';
  if(decoder.split('#ifdef __ANDROID__').length!==2||decoder.split(decode).length!==2)throw Error('Original NDK decoder boundary changed');
  decoder='#if defined(__APPLE__)\n#define lseek64 lseek\n#endif\n'+decoder.replace('#ifdef __ANDROID__','#if defined(__ANDROID__) || defined(__APPLE__)')
    .replace('#include <SkImageInfo.h>','#include <SkImageInfo.h>\n#include <src/core/SkConvertPixels.h>')
    .replace(decode,[
      '    const bool publicRgba = info.colorType() == kBGRA_8888_SkColorType;',
      '    const auto rgbaInfo = info.makeColorType(kRGBA_8888_SkColorType);',
      '    if (publicRgba && imageDecoder->currentFrame() != 0 &&',
      '        !SkConvertPixels(info, pixels, stride, rgbaInfo, pixels, stride))',
      '        return ANDROID_IMAGE_DECODER_INTERNAL_ERROR;',decode,
      '    if (publicRgba && !SkConvertPixels(rgbaInfo, pixels, stride, info, pixels, stride))',
      '        return ANDROID_IMAGE_DECODER_INTERNAL_ERROR;'].join('\n'));
  const decoderSource=path.join(output,'imagedecoder.cpp');fs.writeFileSync(decoderSource,decoder);
  // GraphicsJNI declares Canvas only by pointer and already forward-declares it.
  // Its incidental Canvas implementation include pulls device-only resource
  // headers into this host NDK module; retain every declaration without it.
  const graphicsHeader=fs.readFileSync(path.join(hwui,'jni/GraphicsJNI.h'),'utf8');
  fs.writeFileSync(path.join(output,'GraphicsJNI.h'),graphicsHeader.replace('#include <hwui/Canvas.h>','#include <SkRegion.h>'));
  const objects=[];
  for(const [index,source]of [ndk,adapted,path.join(root,'pixel-layout.cpp'),assets,path.join(sources.framework,units[3]),path.join(root,'asset-files.cpp'),decoderSource,path.join(sources.framework,units[5])].entries()){
    const object=path.join(output,`bitmap-${index}.o`);run('clang++',[...flags,'-c',source,'-o',object]);objects.push(object);
  }
  const definitions=new Set(objects.flatMap(object=>read('nm',['-gjU',object]).split('\n')));
  const undefinedNames=[...new Set(objects.flatMap(object=>read('nm',['-ju',object]).split('\n')))].filter(name=>name&&!definitions.has(name));
  const probe=path.join(output,'system-symbols');
  run('clang++',['-std=c++20',...nativePathFlags({build:output}),path.join(root,'system-symbols.cpp'),'-o',probe]);
  const system=new Set(execFileSync(probe,[],{input:undefinedNames.map(name=>name.slice(1)).join('\n')+'\n',encoding:'utf8'}).trim().split('\n').map(name=>'_'+name));
  for(const dependency of [binder.library,processLibrary])for(const name of read('nm',['-gjU',dependency]).split('\n'))system.add(name);
  const available=publisherFunctions(publisher);
  const bindings=undefinedNames.filter(name=>available.has(name)&&!system.has(name)).sort().map(name=>({name,offset:available.get(name)}));
  for(const binding of bindings)if(binding.offset===null)throw Error('Ambiguous publisher dependency: '+binding.name);
  const exports=[...definitions].filter(name=>/^_(AndroidBitmap|ABitmap|AAsset|AConfiguration|AImageDecoder)/.test(name)&&!name.endsWith('_original')).sort();
  const header=[
    'extern "C" uintptr_t publisherTargets[];',
    `static constexpr uintptr_t publisherOffsets[]={${bindings.map(binding=>binding.offset).join(',')}};`,
    `static const char* publisherNames[]={${bindings.map(binding=>JSON.stringify(binding.name.slice(1))).join(',')}};`,
    ...exports.map((name,index)=>`extern "C" void ndk_export_${index}() asm(${JSON.stringify(name)});`),
    'static const struct {const char* name;void* address;} ndkExports[]={',
    ...exports.map((name,index)=>`{${JSON.stringify(name.slice(1))},reinterpret_cast<void*>(&ndk_export_${index})},`),'};','',
  ].join('\n');
  fs.writeFileSync(path.join(output,'publisher-bindings.h'),header);
  const assembly=['.section __DATA,__data','.p2align 3','.globl _publisherTargets','_publisherTargets:',`.space ${bindings.length*8}`,'.text'];
  bindings.forEach(({name},index)=>assembly.push('.p2align 2',`.private_extern ${name}`,`${name}:`,
    'adrp x16, _publisherTargets@PAGE',`add x16, x16, _publisherTargets@PAGEOFF`,`ldr x16, [x16, #${index*8}]`,'br x16'));
  fs.writeFileSync(path.join(output,'publisher-bindings.S'),assembly.join('\n')+'\n');
  const exportsFile=path.join(output,'exports.txt');fs.writeFileSync(exportsFile,['_JNI_OnLoad',...exports].join('\n')+'\n');
  const library=path.join(output,'libandroid-ndk-graphics.dylib');
  run('clang++',[...flags,'-dynamiclib','-Wl,-install_name,@rpath/libandroid-ndk-graphics.dylib',
    '-Wl,-exported_symbols_list,'+exportsFile,path.join(root,'bridge.cpp'),path.join(output,'publisher-bindings.S'),...objects,
    binder.library,processLibrary,'-Wl,-rpath,@loader_path','-o',library]);
  for(const name of ['libandroid-binder-rpc.dylib','libklepton-jvm.dylib'])
    run('install_name_tool',['-change','@rpath/'+name,'@loader_path/'+name,library]);
  const licenses=path.join(output,'licenses');
  addLicense(path.dirname(licenses),'ndk-graphics',path.join(sources.framework,'NOTICE'));
  for(const relative of fs.globSync(['**/LICENSE','**/LICENSE.*','**/NOTICE','**/NOTICE.*'],{cwd:sources.skia})){
    addLicense(path.dirname(licenses),'ndk-graphics',path.join(sources.skia,relative));
  }

  const result={library,licenses,bindings,exports};
  fs.writeFileSync(path.join(output,'build.json'),JSON.stringify(result,null,2)+'\n');return result;
}
