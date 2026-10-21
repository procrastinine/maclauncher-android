import {mergeSanitizers,sanitizerFlags} from './sanitizers.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {execFileSync,spawnSync} from 'node:child_process';
import {parseBlueprint} from '../upstream/blueprint.mjs';

export async function buildStatics(ctx) {
const output=path.join(ctx.work,'module/components/native-static'),project=ctx.work,source=path.join(ctx.work,'module/source'),repo=ctx.work;fs.mkdirSync(output,{recursive:true});
const options={...ctx.options,ninja:ctx.tools.ninja,'blueprint-parser':ctx.tools.blueprintParser};
const inputs=JSON.parse(fs.readFileSync(path.join(project,'core-object-inputs.json'))),compiler=ctx.tools.clangxx;
const sourceFiles=['native/libs/ui/Android.bp','native/libs/math/Android.bp','native/libs/arect/Android.bp','native/opengl/libs/Android.bp','framework/libs/protoutil/Android.bp','native/libs/shaders/Android.bp','native/libs/tonemap/Android.bp','core/libstats/pull_lazy/Android.bp','core/libstats/socket_lazy/Android.bp','webp/Android.bp','wuffs/Android.bp','giflib/Android.bp'].map(file=>path.join(source,file));
sourceFiles.push(path.join(options['nativehelper-source'],'Android.bp'),path.join(options['ziparchive-source'],'Android.bp'),path.join(options['incfs-source'],'incfs/Android.bp'));
const modules=new Map();for(const file of sourceFiles){for(const record of parseBlueprint(fs.readFileSync(file,'utf8'),{file,parser:options['blueprint-parser']}).modules){const {name}=record.properties;if(name)modules.set(name,{...record.properties,type:record.type,directory:path.dirname(file),file});}}
const externalDefaults=new Set(['android.hardware.graphics.common-ndk_shared','android.hardware.graphics.composer3-ndk_shared']);
const arrays=['srcs','exclude_srcs','cflags','cppflags','local_include_dirs','include_dirs','export_include_dirs','static_libs','whole_static_libs','shared_libs','header_libs','generated_sources','generated_headers'];
const resolve=(name,stack=[])=>{
 if(stack.includes(name))throw Error('Cyclic defaults '+name);const record=modules.get(name);if(!record)throw Error('Missing original module '+name);
 const result=Object.fromEntries(arrays.map(key=>[key,[]]));result.sanitize={};
 const append=(value,dir)=>{result.sanitize=mergeSanitizers(result.sanitize,value.sanitize||{});for(const key of arrays)for(const item of value[key]||[])result[key].push({value:item,dir});for(const key of ['c_std','cpp_std'])if(value[key])result[key]=value[key];};
 for(const dependency of record.defaults||[]){if(externalDefaults.has(dependency))continue;const base=resolve(dependency,[...stack,name]);result.sanitize=mergeSanitizers(result.sanitize,base.sanitize);for(const key of arrays)result[key].push(...base[key]);for(const key of ['c_std','cpp_std'])if(base[key])result[key]=base[key];}
 append(record,record.directory);for(const value of [record.target?.not_windows,record.target?.linux,record.target?.linux_bionic,record.target?.android,record.arch?.arm64,record.multilib?.lib64])if(value)append(value,record.directory);
 return {...result,name,type:record.type,file:record.file,defaults:record.defaults||[]};
};
const requested=['libui-types','libmath','libarect','libEGL_blobCache','libprotoutil','libshaders','libstatspull_lazy','libstatssocket_lazy','libtonemap','libwebp-decode','libwebp-encode','libwuffs_mirror_release_c','libgif','libnativehelper_lazy','libziparchive_for_incfs','libincfs-utils'];
const records=[],seen=new Set();function add(name){if(seen.has(name))return;seen.add(name);const record=resolve(name);records.push(record);for(const {value}of [...record.static_libs,...record.whole_static_libs]){if(modules.has(value))add(value);else throw Error('Missing actual static dependency '+value+' from '+name);}}
requested.forEach(add);
const extraIncludes=[...['header_only_include','include','include_jni','include_platform','include_platform_header_only'].map(p=>path.join(options['nativehelper-source'],p)),path.join(options['ziparchive-source'],'include'),path.join(options['ziparchive-source'],'incfs_support/include'),path.join(options['incfs-source'],'incfs/include'),path.join(options['incfs-source'],'incfs/kernel-headers'),path.join(options['incfs-source'],'incfs/util/include')];
for(const record of records)for(const {value,dir}of [...record.export_include_dirs,...record.local_include_dirs])extraIncludes.push(path.resolve(dir,value));
const quote=value=>"'"+String(value).replaceAll("'","'\\''")+"'";
const ninja=[`cxx = ${quote(compiler)}`,`cc = ${quote(path.join(path.dirname(compiler),'clang'))}`,`ar = ${quote(path.join(path.dirname(compiler),'llvm-ar'))}`,
 'rule cxx\n  command = $cxx $flags -MMD -MF $out.d -c $in -o $out\n  depfile = $out.d\n  deps = gcc\n  description = CXX $in',
 'rule cc\n  command = $cc $flags -MMD -MF $out.d -c $in -o $out\n  depfile = $out.d\n  deps = gcc\n  description = CC $in',
 'rule archive\n  command = rm -f $out && $ar rcs $out $in\n  description = AR $out'];
const resolved=[];for(const record of records){
 const exclusions=new Set(record.exclude_srcs.map(({value,dir})=>path.resolve(dir,value))),sources=[];
 for(const {value,dir}of record.srcs){if(value.startsWith(':'))throw Error('Unresolved original generated source '+record.name+':'+value);for(const file of /[*?[]/.test(value)?fs.globSync(value,{cwd:dir}):[value]){const actual=path.resolve(dir,file);if(exclusions.has(actual))continue;if(!fs.existsSync(actual))throw Error('Missing original source '+actual);sources.push(actual);}}
 if(record.generated_sources.length)throw Error('Original generated source owner needed '+record.name+':'+JSON.stringify(record.generated_sources));
 const objects=[];for(const file of [...new Set(sources)]){
  const object=path.join(output,'objects',record.name,path.relative(repo,file).replaceAll('/','_')+'.o');fs.mkdirSync(path.dirname(object),{recursive:true});objects.push(object);
  const isC=file.endsWith('.c');const compatibilityFlags=record.name==='libui-types'?['-Wno-error=deprecated-declarations']:record.name==='libziparchive_for_incfs'?['-Wno-error=reorder-init-list']:[];
  const flags=[...inputs.flags,...(!isC?inputs.cppDefaults:[]),...extraIncludes.flatMap(dir=>['-I',dir]),...record.local_include_dirs.flatMap(({value,dir})=>['-I',path.resolve(dir,value)]),...record.include_dirs.flatMap(({value})=>['-I',path.join(source,value)]),...record.cflags.map(r=>r.value),...(!isC?record.cppflags.map(r=>r.value):[]),'-std='+(isC?record.c_std||'gnu11':record.cpp_std||'gnu++2b'),...compatibilityFlags,...sanitizerFlags(record.sanitize,ctx)];
  ninja.push(`build ${object}: ${isC?'cc':'cxx'} ${file}`,'  flags = '+flags.map(quote).join(' '));
 }
 const archive=path.join(output,record.name+'.a');ninja.push(`build ${archive}: archive ${objects.join(' ')}`);
 resolved.push({...record,sources:[...new Set(sources)],objects,archive});
}
ninja.push('default '+resolved.map(record=>record.archive).join(' '));fs.writeFileSync(path.join(output,'build.ninja'),ninja.join('\n')+'\n');
fs.writeFileSync(path.join(output,'resolved-inputs.json'),JSON.stringify({requested,externalDefaults:[...externalDefaults],commonFlags:inputs.flags,extraIncludes,records:resolved},null,2)+'\n');
const result=spawnSync(options.ninja,['-C',output,'-j','3','-k','12'],{stdio:'inherit'});if(result.status!==0)throw Error('Original HWUI static dependency compile failed: '+result.status);

}
