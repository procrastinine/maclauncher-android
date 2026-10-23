import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';
import {pathToFileURL} from 'node:url';
import {prepareSourceTree,copyChanged,writeChanged} from '../upstream/source-files.mjs';
import {nativePathFlags} from '../upstream/native-paths.mjs';
import {exportSources} from '../binder-rpc/build.mjs';

const owned=import.meta.dirname,sha=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
/** Original linux_bionic linker, including its original no-malloc/no-EH static
 * closure. Source checkouts and supplier archives remain read-only. No network
 * acquisition or stock image replacement occurs in this component builder. */
export function buildPreparedLinker({inputsFile,output,execute=false}){
 const inputs=JSON.parse(fs.readFileSync(inputsFile,'utf8')),base=path.dirname(path.resolve(inputsFile));
 if(inputs.schemaVersion!==1)throw Error('Unsupported linker inputs');
 const resolveInput=file=>path.resolve(base,file);
 const artifact=value=>{const file=resolveInput(value.file);if(!fs.lstatSync(file).isFile()||sha(file)!==value.sha256)throw Error('Linker archive identity changed');return file;};
 const sourceManifest=path.join(owned,'linker-sources.json'),registered=JSON.parse(fs.readFileSync(sourceManifest));
 for(const name of ['armOptimizedRoutines','llvmLibc'])if(inputs.sources[name]?.revision!==registered.sources[name]?.revision)throw Error('Registered linker source revision differs: '+name);
 const pin='09a271af557444c9a6b3f3146d6d474156fd6cdb',source=path.join(output,'source'),out=path.join(output,'build'),gen=path.join(out,'gen'),ndk=resolveInput(inputs.ndk),tools=path.join(ndk,'toolchains/llvm/prebuilt/darwin-x86_64'),parser=resolveInput(inputs.blueprintParser);
 const quoted=s=>"'"+s.replaceAll("'","'\\''")+"'",ninja=s=>s.replaceAll('$','$$').replaceAll(' ','$ ').replaceAll(':','$:');
 const write=writeChanged,patches=['linker-preconstructor.patch','linker-dlopen-owner.patch'].map(name=>path.join(owned,name));
 fs.mkdirSync(source,{recursive:true});fs.mkdirSync(gen,{recursive:true});
 prepareSourceTree(path.join(source,'bionic'),temporary=>{
  exportSources(resolveInput(inputs.bionicSource),pin,['linker','libc','libdl','libstdc++','libm'],temporary);
  for(const file of patches)execFileSync('git',['apply','--unsafe-paths',file],{cwd:temporary,stdio:'inherit',env:{...process.env,GIT_CEILING_DIRECTORIES:path.dirname(temporary)}});
  copyChanged(path.join(owned,'preconstructor-abi.h'),path.join(temporary,'linker/preconstructor-abi.h'));
 });
 for(const [key,destination]of Object.entries({core:'system/core',libbase:'system/libbase',logging:'system/logging',ziparchive:'system/libziparchive',fmt:'external/fmtlib',armOptimizedRoutines:'external/arm-optimized-routines',llvmLibc:'external/llvm-libc'})){
  const entry=inputs.sources[key];if(!entry?.directory||!entry.revision)throw Error('Missing original linker source '+key);
  const directory=resolveInput(entry.directory),git=path.join(directory,'.git');
  if(fs.existsSync(git)&&execFileSync('git',['-C',directory,'rev-parse','HEAD'],{encoding:'utf8'}).trim()!==entry.revision)throw Error('Original linker source pin differs '+key);
  const target=path.join(source,destination);fs.mkdirSync(path.dirname(target),{recursive:true});const before=fs.lstatSync(target,{throwIfNoEntry:false});
  if(before&&(!before.isSymbolicLink()||fs.realpathSync(target)!==fs.realpathSync(directory)))throw Error('Linker source projection differs '+key);
  if(!before)fs.symlinkSync(directory,target);
 }
 const env={...process.env,PYTHONDONTWRITEBYTECODE:'1'};
 write(path.join(gen,'syscalls-arm64.S'),execFileSync(resolveInput(inputs.python),[path.join(source,'bionic/libc/tools/gensyscalls.py'),'arm64',path.join(source,'bionic/libc/SYSCALLS.TXT')],{env}));
 write(path.join(gen,'generated_android_ids.h'),execFileSync(resolveInput(inputs.python),[resolveInput(inputs.fsConfigGenerator),'aidarray',path.join(source,'system/core/libcutils/include/private/android_filesystem_config.h')],{env}));
 const libcxx=resolveInput(inputs.libcxxHeaders),config=fs.readFileSync(artifact(inputs.libcxxConfig),'utf8');
 if(!config.includes('#define _LIBCPP_ABI_NAMESPACE __1'))throw Error('Original no-EH platform namespace changed');
 write(path.join(gen,'__config_site'),config);
const docs=['bionic/libdl/Android.bp','bionic/linker/Android.bp','bionic/libc/Android.bp','bionic/libc/async_safe/Android.bp','bionic/libc/system_properties/Android.bp','bionic/libm/Android.bp','external/arm-optimized-routines/Android.bp','external/llvm-libc/Android.bp','system/libbase/Android.bp','system/logging/liblog/Android.bp','system/libziparchive/Android.bp','external/fmtlib/Android.bp','system/core/property_service/libpropertyinfoparser/Android.bp'].filter(file=>fs.existsSync(path.join(source,file)));
const parsed=JSON.parse(execFileSync(parser,[],{input:JSON.stringify(docs.map(file=>({file,source:fs.readFileSync(path.join(source,file),'utf8'),parents:[]}))),encoding:'utf8',maxBuffer:8*1024*1024}));
const modules=new Map();
for(let i=0;i<parsed.length;i++){
 if(parsed[i].error)throw Error(parsed[i].error);
 for(const module of parsed[i].modules)if(module.properties.name&&module.type!=='ndk_library')modules.set(module.properties.name,{...module.properties,directory:path.dirname(docs[i])});
}
function merge(a,b){for(const [key,value] of Object.entries(b)){if(Array.isArray(value))a[key]=[...(a[key]??[]),...value];else if(typeof value!=='object'||value===null)a[key]=value;}return a;}
function resolve(name,seen=new Set()){
 if(seen.has(name))throw Error('Blueprint defaults cycle: '+name);
 const own=modules.get(name);if(!own){if(name==='linux_bionic_supported')return {};throw Error('Missing original module: '+name);}
 const result={};for(const parent of own.defaults??[])merge(result,resolve(parent,new Set([...seen,name])));
 merge(result,own);
 // The JVM embedding uses upstream's supported Linux-Bionic debugger/signal
 // owner. Dependencies retain the original Android image's Bionic ABI.
 const osTarget=name==='linker_binary_defaults'?own.target?.linux_bionic:own.target?.android;
 for(const value of [own.arch?.arm64,osTarget,own.target?.android_arm64,own.target?.linux,own.target?.bionic,own.target?.not_windows,own.target?.android64,own.multilib?.lib64,own.static])if(value)merge(result,value);
 return result;
}
function sourceList(name){
 if(name==='syscalls-arm64')return ['../build/gen/syscalls-arm64.S'];
 const mod=resolve(name),excluded=new Set((mod.exclude_srcs??[]).map(file=>path.join(mod.directory,file)));
 return [...new Set((mod.srcs??[]).flatMap(file=>file.startsWith(':')?sourceList(file.slice(1)):file.includes('*')?fs.globSync(path.join(mod.directory,file),{cwd:source}):path.join(mod.directory,file)))].filter(file=>!excluded.has(file));
}
const baseFlags=['--target=aarch64-linux-android35','-O2','-gline-tables-only','-fPIC','-ffunction-sections','-fdata-sections','-fno-stack-protector','-fno-exceptions','-fno-rtti','-fno-strict-aliasing','-DANDROID','-D__ANDROID_APEX__','-DNDEBUG','-D__ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__','-D__BIONIC_NO_PAGE_SIZE_MACRO','-D__LIBC_API__=10000','-D__LIBM_API__=10000','-D__LIBDL_API__=10000','-Wno-unknown-warning-option'];
const includes=[owned,gen,...['bionic/libc','bionic/libc/platform','bionic/libc/async_safe/include','bionic/libc/system_properties/include','bionic/libdl/include_private','system/core/debuggerd/include','system/core/libcutils/include','system/core/libsystem/include','system/libbase/include','system/logging/liblog/include','system/libziparchive/include','system/libziparchive/incfs_support/include','external/fmtlib/include','system/core/property_service/libpropertyinfoparser/include'].map(file=>path.join(source,file))];
// Original platform headers are required for linker-private interfaces, not NDK stubs.
const systemIncludes=['bionic/libc/include','bionic/libc/kernel/uapi/asm-arm64','bionic/libc/kernel/uapi','bionic/libc/kernel/android/scsi','bionic/libc/kernel/android/uapi'].flatMap(file=>['-isystem',path.join(source,file)]);
const groups=['linker','liblinker_main','liblinker_malloc','liblinker_debuggerd_stub','libdl_static','libc_bootstrap','libc_init_static','libc_dynamic_dispatch'];
const baseGroups=[...groups],extended=true;
if(extended)groups.push(...[...resolve('libc_common').whole_static_libs,'libc_unwind_static','libsystemproperties','libpropertyinfoparser','llvmlibc','libbase','fmtlib','liblog_for_runtime_apex','libziparchive'].filter(name=>!groups.includes(name)&&modules.has(name)));
const objectGroups={},commands=[];
const lines=['ninja_required_version = 1.10','rule compile','  command = $compiler $flags -MMD -MF $out.d -c $in -o $out','  depfile = $out.d','  deps = gcc','  description = COMPILE $in','rule archive','  command = '+quoted(path.join(tools,'bin/llvm-ar'))+' rcs $out $in','  description = ARCHIVE $out'];
for(const name of groups){
 const mod=resolve(name),sources=sourceList(name),objects=[];
 for(const file of sources){
  const input=path.join(source,file);if(!fs.existsSync(input))throw Error('Original source unavailable: '+file);
  const obj=path.join(out,'obj',name,file.replaceAll('/','_')+'.o');fs.mkdirSync(path.dirname(obj),{recursive:true});objects.push(obj);
  const assembly=file.endsWith('.S'),c=file.endsWith('.c');
  const compiler=path.join(tools,'bin',c||assembly?'clang':'clang++');
  const useCxxHeaders=!c&&!assembly&&!name.startsWith('libc_')&&!['libdl_static','libstdc++','libsystemproperties','libasync_safe','libpropertyinfoparser','llvmlibc'].includes(name);
  const localIncludes=[...(mod.local_include_dirs??[]).map(file=>path.join(source,mod.directory,file)),...(mod.include_dirs??[]).map(file=>path.join(source,file))];
  if(name==='liblog_for_runtime_apex')localIncludes.push(path.join(source,'system/core/libutils/include'),path.join(source,'system/core/libutils/binder/include'));
  if(name==='libziparchive'&&inputs.googletestHeaders)localIncludes.push(resolveInput(inputs.googletestHeaders));
  const splitFlags=flags=>flags.flatMap(flag=>flag.startsWith('-include ')?flag.split(' '):[flag]);
  const flags=[...baseFlags,...nativePathFlags({source,build:out,roots:[[resolveInput(inputs.ndk),'/toolchain/ndk']]}),...(assembly?['-D__ASSEMBLY__']:c?['-std=gnu99']:['-std=gnu++20','-nostdinc++']),...splitFlags((mod.cflags??[]).filter(flag=>flag!=='-Werror')),...splitFlags(assembly?mod.asflags??[]:c?mod.conlyflags??[]:mod.cppflags??[]),...[...localIncludes,...includes].map(file=>'-I'+file),...(useCxxHeaders?['-isystem',libcxx]:[]),...systemIncludes];
  lines.push('build '+ninja(obj)+': compile '+ninja(input),'  compiler = '+quoted(compiler),'  flags = '+flags.map(quoted).join(' '));
  commands.push({module:name,source:input,sha256:sha(input),object:obj,command:[compiler,...flags,'-c',input,'-o',obj]});
 }
 objectGroups[name]=objects;
  if(name!=='linker'){
   const members=[...objects];
   if(name==='libbase'&&extended)for(const file of sourceList('fmtlib'))members.push(path.join(out,'obj/fmtlib',file.replaceAll('/','_')+'.o'));
   lines.push('build '+ninja(path.join(out,name+'.a'))+': archive '+members.map(ninja).join(' '));
  }
}
if(extended){
 const nomallocGroups=[...resolve('libc_common').whole_static_libs,'libc_init_static','libc_unwind_static','libsystemproperties','libpropertyinfoparser','llvmlibc'];
 const members=nomallocGroups.flatMap(name=>objectGroups[name]??[]);
 lines.push('build '+ninja(path.join(out,'libc_nomalloc.a'))+': archive '+members.map(ninja).join(' '));
}
const archiveNames=[...new Set(resolve('linker').static_libs)].filter(name=>!objectGroups[name]);
const archiveConfig=Object.fromEntries(Object.entries(inputs.archives).map(([name,value])=>[name,artifact(value)]));
archiveConfig.libc_nomalloc=path.join(out,'libc_nomalloc.a');
const required=archiveNames.map(name=>({name,provided:archiveConfig[name]??null}));
const missing=required.filter(item=>!item.provided||(!fs.existsSync(item.provided)&&!(extended&&item.name==='libc_nomalloc'&&item.provided===path.join(out,'libc_nomalloc.a'))));
lines.push('build objects: phony '+[...objectGroups.linker,...groups.filter(name=>name!=='linker').map(name=>path.join(out,name+'.a')),...(extended?[path.join(out,'libc_nomalloc.a')]:[])].map(ninja).join(' '));
if(!missing.length){
 const raw=path.join(out,'linker64.unprefixed'),final=path.join(out,'linker64');
 const libraries=[...new Set(resolve('linker').static_libs)].map(name=>objectGroups[name]?path.join(out,name+'.a'):path.resolve(archiveConfig[name]));
 const icu={library:{path:artifact(inputs.icu),sha256:inputs.icu.sha256},wrapper:{path:path.join(source,'bionic/libc/bionic/icu4x.rs'),sha256:inputs.icu.wrapperSha256}};
 if(sha(icu.library.path)!==icu.library.sha256||sha(icu.wrapper.path)!==icu.wrapper.sha256)throw Error('Original ICU wrapper/build artifact drift');
 libraries.push(icu.library.path,path.join(out,'libdl_static.a'));
 const builtins=execFileSync(path.join(tools,'bin/clang'),['--target=aarch64-linux-android35','-print-libgcc-file-name'],{encoding:'utf8'}).trim();libraries.push(builtins);
 // No crtbegin/ordinary libc/libc++ implicit dependencies: original linker owns bootstrap.
 const ldflags=['--target=aarch64-linux-android35','-nostdlib','-shared','-Wl,-Bsymbolic','-Wl,--exclude-libs,ALL','-Wl,-soname,ld-android.so','-Wl,--pack-dyn-relocs=relr','-Wl,--hash-style=both','-Wl,-z,max-page-size=16384','-Wl,--build-id=sha1','-Wl,-z,defs','-Wl,--gc-sections','-Wl,--version-script='+path.join(source,'bionic/linker/linker.generic.map')];
 lines.push('rule link','  command = '+quoted(path.join(tools,'bin/clang++'))+' '+ldflags.map(quoted).join(' ')+' -o $out '+objectGroups.linker.map(quoted).join(' ')+' -Wl,--start-group '+libraries.map(quoted).join(' ')+' -Wl,--end-group','rule prefix','  command = '+quoted(path.join(tools,'bin/llvm-objcopy'))+' --prefix-symbols=__dl_ $in $out','build '+ninja(raw)+': link '+[...objectGroups.linker,...libraries].map(ninja).join(' '),'build '+ninja(final)+': prefix '+ninja(raw),'build linker: phony '+ninja(final));
}
write(path.join(out,'build.ninja'),lines.join('\n')+'\n');
const plan={schemaVersion:1,sourcePin:pin,patches:patches.map(file=>({path:path.basename(file),sha256:sha(file)})),linkerTarget:'linux_bionic',icu:{sha256:inputs.icu.sha256,source:inputs.icu.source},sourceManifestSha256:sha(sourceManifest),objects:commands,originalStaticDependencies:required,missingStaticDependencies:missing.map(item=>item.name)};
write(path.join(out,'build-plan.json'),JSON.stringify(plan,null,2)+'\n');

 if(execute){
  if(missing.length)throw Error('Original linker static variants unavailable: '+missing.map(item=>item.name).join(', '));
  execFileSync(resolveInput(inputs.ninja),['-f',path.join(out,'build.ninja'),'-j4','--','linker'],{stdio:'inherit'});
 }
 return {plan,library:path.join(out,'linker64')};
}
if(process.argv[1]&&import.meta.url===pathToFileURL(path.resolve(process.argv[1])).href){
 const args=process.argv.slice(2),options={};for(let i=0;i<args.length;i+=2){if(!args[i].startsWith('--')||!args[i+1])throw Error('Expected --inputs FILE --output DIRECTORY --execute true|false');options[args[i].slice(2)]=args[i+1];}
 const result=buildPreparedLinker({inputsFile:path.resolve(options.inputs),output:path.resolve(options.output),execute:options.execute==='true'});console.log(JSON.stringify({library:result.library,objects:result.plan.objects.length,executed:options.execute==='true'}));
}
