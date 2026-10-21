import fs from 'node:fs';
import path from 'node:path';
import {spawnSync} from 'node:child_process';

export async function buildCore(ctx) {
const root=ctx.work,output=path.join(root,'module'),source=path.join(output,'source');
const targets=JSON.parse(fs.readFileSync(new URL('./targets.json',import.meta.url))).targets;
const existing={compiler:ctx.tools.clangxx,args:ctx.includes.flatMap(dir=>['-I',dir])};
// Every HWUI translation unit must see the same complete class definition.
// Preserve the complete source tree and overlay only the two owned files.
const originalHwui=path.join(source,'framework/libs/hwui');
const adaptedHwui=path.join(output,'adapted/framework/libs/hwui');
if(!fs.existsSync(adaptedHwui)||fs.lstatSync(adaptedHwui).isSymbolicLink())throw Error('The owned HWUI source adaptation must be prepared before compilation');
for(const name of ['CommonPool.h','CommonPool.cpp']){
 const bytes=fs.readFileSync(path.join(root,'adapted/thread',name));
 const dest=path.join(adaptedHwui,'thread',name);
 if(!fs.existsSync(dest)||!fs.readFileSync(dest).equals(bytes))fs.writeFileSync(dest,bytes);
}
const includes=[];
for(let i=0;i<existing.args.length;i++)if(existing.args[i]==='-I')includes.push(existing.args[++i].replace(originalHwui,adaptedHwui));
includes.push(...['src','aconfig/hwui/include','aconfig/surfaceflinger/include','aconfig/accessibility/include','aconfig/text/include','aconfig/egl/include','aconfig/gui/include'].map(d=>path.join(output,'gen',d)),
 path.join(source,'wuffs/release/c'),path.join(source,'statsd/lib/libstatspull/include'),path.join(source,'statsd/lib/libstatssocket/include'),
 path.join(source,'protobuf/src'),path.join(source,'perfetto/include'),path.join(source,'perfetto/include/perfetto/base/build_configs/android_tree'));
includes.push(...ctx.includes);
const quote=arg=>"'"+String(arg).replaceAll("'","'\\''")+"'";
const semanticDefaults=['-DNDEBUG','-UDEBUG','-D__compiler_offsetof=__builtin_offsetof','-D__ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__',
 '-faddrsig','-ffp-contract=off','-fno-strict-aliasing','-ffunction-sections','-fdata-sections','-fno-short-enums',
 '-funwind-tables','-fstack-protector-strong','-Wa,--noexecstack','-D_FORTIFY_SOURCE=2'];
const flags=['--target=aarch64-linux-android10000','-fPIC','-O2','-g','-DANDROID','-D_LIBCPP_ENABLE_THREAD_SAFETY_ANNOTATIONS',
 ...semanticDefaults,
 '-Wno-unknown-warning-option','-Wno-inconsistent-missing-override','-Wno-unused-const-variable','-ffile-prefix-map='+root+'=/src/android-hwui',
 ...[...new Set(includes)].flatMap(dir=>['-I',dir])];
const cppDefaults=['-fno-rtti','-fno-exceptions','-fvisibility-inlines-hidden'];
const ninja=['cxx = '+quote(existing.compiler),
 'rule cxx\n  command = $cxx $flags -MMD -MF $out.d -c $in -o $out\n  depfile = $out.d\n  deps = gcc\n  description = CXX $in'];
const records=[];
for(const [name,target]of Object.entries(targets)){
 const base=name==='libhwui'?adaptedHwui:path.join(source,'skia');
 const sources=target.srcs.filter(file=>/\.(cpp|cc|c|S)$/.test(file)&&!target.exclude_srcs.includes(file));
 // Additional declared header dependencies are limited to their consuming
 // translation units.  Appending new, previously absent header names leaves
 // already compiled unrelated units and their recorded dependencies intact.
 const extraIncludesBySource=name==='libhwui'?Object.fromEntries(['renderthread/CanvasContext.cpp','WebViewFunctorManager.cpp'].map(file=>[file,[path.join(output,'components/native-hidl/include'),path.join(output,'components/native-gui/include')]])):
  {'src/xml/SkXMLParser.cpp':[path.join(source,'expat/expat/lib')]};
 const objects=[];
 for(const file of sources){
  const input=path.join(base,file);
  if(!fs.existsSync(input))throw Error('Original source is absent: '+input);
  const object=path.join(output,'objects',name,file.replaceAll('/','_')+'.o');fs.mkdirSync(path.dirname(object),{recursive:true});
  objects.push(object);const local=target.local_include_dirs.map(dir=>'-I'+path.join(base,dir));
  const options=[...flags,...cppDefaults,'-std='+(name==='libhwui'?'gnu++2b':'gnu++17'),...target.cflags,...target.cppflags,...local,...(extraIncludesBySource[file]||[]).map(dir=>'-I'+dir),...(name==='libhwui'&&file==='thread/CommonPool.cpp'?['-fexceptions']:[]),'-Wno-unused-const-variable','-Wno-unused-variable','-Wno-error=vla-cxx-extension'];
  ninja.push('build '+object+': cxx '+input,'  flags = '+options.map(quote).join(' '));
 }
 records.push({name,sources:sources.length,objects,cflags:target.cflags,cppflags:target.cppflags,localIncludeDirs:target.local_include_dirs,extraIncludesBySource});
 ninja.push('build '+name+'-objects: phony '+objects.join(' '));
}
ninja.push('default libhwui-objects libskia-objects');
fs.writeFileSync(path.join(output,'build.ninja'),ninja.join('\n')+'\n');
fs.writeFileSync(path.join(root,'core-object-inputs.json'),JSON.stringify({records,includes,flags,cppDefaults,semanticDefaults,poolExceptionOverride:'thread/CommonPool.cpp only',compilerPolicy:'Original Android16 Soong cc/config/global.go commonGlobalCflags/deviceGlobalCflags/deviceGlobalCppflags and cc/compiler.go default no-RTTI; warning/debug-format controls do not affect runtime semantics.'},null,2)+'\n');
if(ctx.build !== false){
 const command=ctx.tools.ninja;
 const result=spawnSync(command,['-C',output,'-j2','-k8'],{stdio:'inherit'});if(result.status!==0)throw Error("Original HWUI compilation failed: "+result.status);
}else console.log(JSON.stringify(records.map(({name,sources})=>({name,sources}))));

}
