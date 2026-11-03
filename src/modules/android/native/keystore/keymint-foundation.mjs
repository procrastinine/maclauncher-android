import {addLicense} from '../upstream/licenses.mjs';
import {preparePropertyHeaders} from './property-sources.mjs';
import fs from 'node:fs';
import {nativePathFlags} from '../upstream/native-paths.mjs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {createHash} from 'node:crypto';
import {parseBlueprint} from '../upstream/blueprint.mjs';
import {exportSources} from '../binder-rpc/build.mjs';
import {compileHostObject} from '../host-compile.mjs';
import {cachedBuildStep} from '../upstream/build-step.mjs';

const nativeRoot=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const pins=JSON.parse(fs.readFileSync(path.join(nativeRoot,'source-pins.json')));
const suppliers=JSON.parse(fs.readFileSync(new URL('./sources.json',import.meta.url)));
/** Original host KeyMint, libbase, liblog and fmt modules; each unit is built once. */
export async function buildKeyMintFoundation({sources,boringssl,bionic,output,ninja,python}) {
const out=path.resolve(output);
const source=name=>sources[name];
const records=[
  {id:'keymaster',path:source('keymaster'),url:'https://android.googlesource.com/platform/system/keymaster',revision:suppliers.find(supplier=>supplier.name==='keymaster').revision,licenses:['NOTICE']},
  {id:'libcppbor',path:source('libcppbor'),url:'https://android.googlesource.com/platform/system/libcppbor',revision:suppliers.find(supplier=>supplier.name==='libcppbor').revision,licenses:['LICENSE']},
  {id:'libhardware',path:source('libhardware'),url:'https://android.googlesource.com/platform/hardware/libhardware',revision:suppliers.find(supplier=>supplier.name==='libhardware').revision,licenses:['NOTICE']},
  {id:'logging',path:source('logging'),url:'https://android.googlesource.com/platform/system/logging',revision:suppliers.find(supplier=>supplier.name==='logging').revision,licenses:['liblog/NOTICE']},
  {id:'libbase',path:sources.libbase,url:'https://android.googlesource.com/platform/system/libbase',revision:pins.aospLibbase,licenses:['NOTICE'],export:['Android.bp','include','NOTICE','.']},
  {id:'fmt',path:sources.fmt,url:'https://android.googlesource.com/platform/external/fmtlib',revision:pins.aospFmt,licenses:['NOTICE','LICENSE'],export:['Android.bp','include','src','NOTICE','LICENSE']},
  {id:'core',path:sources.core,url:'https://android.googlesource.com/platform/system/core',revision:pins.aospSystemCore,licenses:['libcutils/NOTICE','libutils/NOTICE'],export:['libcutils/include','libprocessgroup/include','libsystem/include','libutils/include','libutils/binder/include','libcutils/NOTICE','libutils/NOTICE']},
];
const run=(cmd,args,opts={})=>execFileSync(cmd,args,{stdio:'inherit',...opts});
const sha=bytes=>createHash('sha256').update(bytes).digest('hex');
fs.mkdirSync(out,{recursive:true});
const dirs={};
for(const r of records){
  const actual=run('git',['-C',r.path,'rev-parse','HEAD'],{encoding:'utf8',stdio:'pipe'}).trim();
  if(actual!==r.revision)throw Error(`Wrong ${r.id} revision ${actual}`);
  r.export??=suppliers.find(supplier=>supplier.name===r.id).trees;
  dirs[r.id]=path.join(out,'source',r.id);
  const exportState=path.join(dirs[r.id],'.export-revision'),exportIdentity=sha(JSON.stringify([r.revision,r.export]));
  if(r.export&&(!fs.existsSync(exportState)||fs.readFileSync(exportState,'utf8')!==exportIdentity)){
    exportSources(r.path,r.revision,r.export,dirs[r.id]);
    fs.writeFileSync(exportState,exportIdentity);
  }
}
const cryptoInclude=boringssl.include;
const cryptoLibrary=boringssl.crypto;
const modules=[];
const units=new Map();
function take(project,file,name){
  const full=path.join(dirs[project],file);
  const module=parseBlueprint(fs.readFileSync(full,'utf8'),{file}).modules.find(m=>m.properties.name===name&&m.type!=='ndk_library');
  if(!module)throw Error(`Missing original module ${name}`);
  const p=module.properties;
  const srcs=[...(p.srcs||[]),...(p.target?.darwin?.srcs||[]),...(p.target?.not_windows?.srcs||[])];
  const excluded=new Set([...(p.exclude_srcs||[]),...(p.target?.darwin?.exclude_srcs||[])]);
  for(const rel of srcs.filter(s=>!excluded.has(s))){
    if(/[:*]/.test(rel))throw Error(`Resolve original expression before compiling: ${rel}`);
    const absolute=path.resolve(path.dirname(full),rel);
    const key=project+'/'+path.relative(dirs[project],absolute);
    if(!fs.existsSync(absolute))throw Error(`Missing original unit ${key}`);
    if(!units.has(key))units.set(key,{key,file:absolute,sha256:sha(fs.readFileSync(absolute)),modules:[]});
    units.get(key).modules.push(name);
  }
  modules.push({project,name,type:module.type,definition:file,line:module.location.line,definitionSha256:sha(fs.readFileSync(full)),properties:p,hostSources:srcs.filter(s=>!excluded.has(s))});
}
// Entire original host component definitions; no algorithm/family source list.
for(const name of ['keymaster_defaults','libkeymaster_messages','libkeymaster_portable','libpuresoftkeymasterdevice_host','libsoft_attestation_cert','libcppcose_rkp'])take('keymaster','Android.bp',name);
take('libcppbor','Android.bp','libcppbor');
for(const name of ['libbase_defaults','libbase'])take('libbase','Android.bp',name);
take('fmt','Android.bp','fmtlib-non-test-defaults');
take('logging','liblog/Android.bp','liblog.defaults');
const property=preparePropertyHeaders({output:path.join(out,'properties'),libbase:dirs.libbase,bionic});
const properties=units.get('libbase/properties.cpp');
properties.originalFile=properties.file;properties.file=property.source;
properties.adaptedSha256=sha(fs.readFileSync(property.source));
const adaptations=property.adaptations;
const includes=[property.include,
  path.join(dirs.keymaster,'include'),path.join(dirs.libcppbor,'include/cppbor'),
  path.join(dirs.libhardware,'include_all'),path.join(dirs.libhardware,'include_vendor'),
  path.join(dirs.libbase,'include'),path.join(dirs.fmt,'include'),
  path.join(dirs.logging,'liblog/include'),path.join(dirs.logging,'liblog'),
  path.join(dirs.core,'libcutils/include'),path.join(dirs.core,'libprocessgroup/include'),
  path.join(dirs.core,'libsystem/include'),path.join(dirs.core,'libutils/include'),path.join(dirs.core,'libutils/binder/include'),cryptoInclude,
];
const flags=[...nativePathFlags({build:path.dirname(out),roots:Object.entries(dirs).map(([name,directory])=>[directory,'/src/'+name])}),'-arch','arm64','-mmacosx-version-min=13.0','-O1','-g','-fPIC','-fno-rtti','-fno-exceptions',
 '-DKEYMASTER_NAME_TAGS','-DLIBLOG_LOG_TAG=1006','-DSNET_EVENT_LOG_TAG=1397638484','-DANDROID_DEBUGGABLE=0',
 ...includes.map(s=>'-I'+s)];
const objects=path.join(out,'objects');fs.mkdirSync(objects,{recursive:true});
const list=[...units.values()];
for(const u of list){
  const name=u.key.replaceAll('/','_')+'.o';u.object=path.join(objects,name);
  compileHostObject({name,inputs:[u.file],flags:[...flags,u.file.endsWith('.c')?'-std=gnu11':'-std=c++20'],
    compiler:u.file.endsWith('.c')?'clang':'clang++',directory:path.join(out,'host-objects'),
    output:u.object,cwd:nativeRoot,ninja,python});
}
const archive=path.join(out,'libkeymint-soft-host.a');
const ar=fs.realpathSync(run('xcrun',['--find','ar'],{encoding:'utf8',stdio:'pipe'}).trim());
const archiveArgs=['rcs',archive,...list.map(u=>u.object)];
await cachedBuildStep({cache:path.join(out,'archive.reuse.json'),inputs:[fileURLToPath(import.meta.url),ar,...list.map(u=>u.object)],
  files:[archive],key:{ar,args:archiveArgs},build:()=>run(ar,archiveArgs)});
const licenses=path.join(out,'licenses');
for(const r of records)for(const f of r.licenses){const original=path.join(dirs[r.id],f);if(fs.existsSync(original))addLicense(output,r.id,original);}
const shared=list.filter(unit=>['libbase/','fmt/','logging/'].some(prefix=>unit.key.startsWith(prefix)));
const result={output:out,sources:dirs,modules,units:list,shared,property,crypto:{include:cryptoInclude,library:cryptoLibrary},archive,flags,licenses};
fs.writeFileSync(path.join(out,'closure.json'),JSON.stringify(result,null,2)+'\n');
return result;
}
