import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import zlib from 'node:zlib';
import {execFileSync} from 'node:child_process';
import {verifyHwuiSource} from './inputs.mjs';
import {isLicenseNoticeFile} from '../upstream/reuse.mjs';
const sha=bytes=>crypto.createHash('sha256').update(bytes).digest('hex');
function relative(file){if(typeof file!=='string'||!file||path.isAbsolute(file)||file.split(/[\\/]/).some(p=>p==='..'||p===''))throw Error('Invalid notice member path');return file;}
function original(ctx,key,check,overrides){
 const record=ctx.originalSources?.[key]?.record,input=overrides?.[key]||ctx.originalSources?.[key]?.input;
 if(!record||!input)throw Error('Original HWUI notice source is required: '+key);
 return verifyHwuiSource({...record,checks:[check]},input).checks[0].file;
}
function lockedPackages(text){
 const result=new Map();for(const block of text.split(/^\[\[package\]\]\s*$/m).slice(1)){
  const get=name=>block.match(new RegExp('^'+name+' = ("(?:[^"\\\\]|\\\\.)*")$','m'))?.[1];const name=get('name'),version=get('version'),source=get('source'),checksum=get('checksum');
  if(!name||!version)throw Error('Cargo lock package identity is incomplete');
  result.set([JSON.parse(name),JSON.parse(version),source?JSON.parse(source):''].join('@'),checksum?JSON.parse(checksum):null);
 }return result;
}
function tarFiles(archive){
 const bytes=zlib.gunzipSync(archive,{maxOutputLength:256<<20}),files=new Map();let at=0,nextName=null,pax={};
 const str=b=>b.subarray(0,b.indexOf(0)<0?b.length:b.indexOf(0)).toString('utf8');
 while(at+512<=bytes.length){const h=bytes.subarray(at,at+512);if(h.every(b=>b===0))break;const raw=str(h.subarray(124,136)).trim();if(!/^[0-7]+$/.test(raw))throw Error('Invalid crate archive size');const size=parseInt(raw,8),type=String.fromCharCode(h[156]||48);at+=512;if(at+size>bytes.length)throw Error('Truncated crate archive');const body=bytes.subarray(at,at+size);at+=Math.ceil(size/512)*512;
  if(type==='L'){nextName=str(body).replace(/\n$/,'');continue;}
  if(type==='x'||type==='g'){let cursor=0;const fields={};while(cursor<body.length){const space=body.indexOf(32,cursor),length=Number(body.subarray(cursor,space).toString());if(space<cursor||!Number.isSafeInteger(length)||length<=space-cursor+1||cursor+length>body.length)throw Error('Invalid crate archive extended header');const entry=body.subarray(space+1,cursor+length-1).toString(),equal=entry.indexOf('=');if(equal>0)fields[entry.slice(0,equal)]=entry.slice(equal+1);cursor+=length;}pax={...pax,...fields};continue;}
  const prefix=str(h.subarray(345,500)),rawName=pax.path||nextName||(prefix?prefix+'/':'')+str(h.subarray(0,100));pax={};nextName=null;
  if(type==='5'){relative(rawName.replace(/\/$/,''));continue;}const name=relative(rawName);if(!['0','2'].includes(type))throw Error('Unsupported crate archive member');if(files.has(name))throw Error('Duplicate crate archive member');files.set(name,type==='2'?{link:str(h.subarray(157,257))}:{bytes:body});
 }return files;
}
function member(files,name){let hops=0;for(;;){const file=files.get(name);if(!file)throw Error('Original crate notice is absent: '+name);if(file.bytes)return file.bytes;if(++hops>16)throw Error('Recursive crate notice alias');name=relative(path.posix.normalize(path.posix.join(path.posix.dirname(name),file.link)));}}
function sourceUnit(ctx,file){
 for(const[key,root]of Object.entries(ctx.sources||{}).sort((a,b)=>b[1].length-a[1].length))if(file.startsWith(root+path.sep))return{source:key,path:path.relative(root,file).split(path.sep).join('/')};
 throw Error('Compiled source notice has no declared original source');
}

/** Cargo build dependencies and procedural macros execute on the build host. */
export function runtimeCargoPackages(meta){
 const nodes=new Map(meta.resolve.nodes.map(node=>[node.id,node])),packages=new Map(meta.packages.map(pkg=>[pkg.id,pkg])),active=new Set();
 function visit(id){
  if(active.has(id)||packages.get(id)?.targets?.some(target=>target.kind?.includes('proc-macro')))return;
  const node=nodes.get(id);if(!node)throw Error('Cargo dependency resolution is incomplete');active.add(id);
  for(const dep of node.deps||[])if(dep.dep_kinds.some(kind=>kind.kind===null))visit(dep.pkg);
 }
 for(const id of meta.workspace_members)visit(id);return meta.packages.filter(pkg=>active.has(pkg.id));
}

/** Rust's copyright page records the licence expressions for its distributed code. */
export function rustLibraryNoticeFiles(root){
 const page=fs.readFileSync(path.join(root,'COPYRIGHT-library.html'),'utf8'),cited=new Set();
 for(const match of page.matchAll(/<b>License:<\/b>([\s\S]*?)<\/p>/g))for(const token of match[1].replace(/<[^>]*>/g,' ').match(/[A-Za-z0-9][A-Za-z0-9.-]*/g)||[])cited.add(token);
 return ['COPYRIGHT-library.html',...fs.readdirSync(path.join(root,'licenses')).sort().filter(name=>cited.has(path.basename(name,'.txt'))).map(name=>'licenses/'+name)];
}
/** Collect original source and toolchain notices without network acquisition.
 * Cargo metadata uses the locked offline dependency graph; registry notice bytes
 * come from archives authenticated by the same Cargo.lock checksums. */
export async function collectHwuiNotices(ctx,{output,cargo={},rust,ndk,sourceFiles,sourceUnits}={}){
 if(!rust?.root||!rust.version||!ndk?.root||!ndk.version)throw Error('Explicit original Rust standard-library and Android NDK notice inputs are required');
 if(!ctx.tools?.cargo||!ctx.tools?.cargoHome)throw Error('Explicit offline Cargo tool and cache inputs are required');
 const sources=Object.values(ctx.originalSources||{}).map(value=>value.record);if(!sources.length)throw Error('Verified original HWUI source inputs are required');
 output=path.resolve(output||path.join(ctx.module,'notices'));if(!ctx.work||!output.startsWith(path.resolve(ctx.work)+path.sep))throw Error('HWUI notices must use the declared work directory');
 const pending=new Map(),origins=[],registry=[],headers=[];
 const add=(file,bytes,origin)=>{file=relative(file);const known=pending.get(file);if(known&&!known.bytes.equals(bytes))throw Error('Notice output has conflicting original contents: '+file);pending.set(file,{bytes,record:{path:file,size:bytes.length,sha256:sha(bytes)}});if(origin&&!origins.some(row=>JSON.stringify(row)===JSON.stringify(origin)))origins.push(origin);};
 for(const record of sources)for(const check of record.noticeChecks||[]){const file=original(ctx,record.key,check,sourceFiles);add(check.output,fs.readFileSync(file),{project:record.project,repository:record.repository,revision:record.revision,licensePath:check.path});}
 let units=sourceUnits;
 if(!units){const input=path.join(ctx.components,'native-static/resolved-inputs.json');if(!fs.existsSync(input))throw Error('Original compiled HWUI source notice inputs are required');const data=JSON.parse(fs.readFileSync(input));units=data.records.flatMap(r=>r.sources).filter(file=>!['webp','wuffs','giflib'].some(key=>file.startsWith(ctx.sources[key]+path.sep))).map(file=>sourceUnit(ctx,file));units.push({source:'logging',path:'rust/logger.rs'},{source:'rustutils',path:'lib.rs'},{source:'av',path:'media/ndk/include/media/NdkImage.h'});}
 for(const unit of [...new Map(units.map(u=>[u.source+'/'+u.path,u])).values()].sort((a,b)=>(a.source+'/'+a.path).localeCompare(b.source+'/'+b.path))){
  const record=sources.find(s=>s.key===unit.source);if(!record)throw Error('Undeclared compiled source notice');const rel=relative(unit.path),cover=record.checks.find(check=>check.path==='.'||check.path===rel||check.type==='tree'&&rel.startsWith(check.path+'/'));if(!cover)throw Error('Compiled source notice is outside original source projection');
  const verified=original(ctx,unit.source,cover),file=cover.type==='tree'?path.join(verified,cover.path==='.'?rel:rel.slice(cover.path.length+1)):verified;
  const comments=[...fs.readFileSync(file,'utf8').slice(0,16000).matchAll(/\/\*[\s\S]*?\*\/|(?:\/\/[^\n]*(?:\n|$))+/g)].map(m=>m[0]).filter(s=>/copyright|licensed under|spdx-license-identifier/i.test(s));
  if(!comments.length)throw Error('Original compiled source attribution is absent: '+unit.source+'/'+rel);const name=record.project+'@'+record.revision+'/'+rel;headers.push(name+'\n'+'='.repeat(name.length)+'\n'+comments.join('\n')+'\n');
 }
 if(headers.length)add('sources/AOSP-source-notices.txt',Buffer.from(headers.join('\n')));
 const manifest=path.resolve(cargo.manifest||path.join(ctx.components,'crabbyavif/rust/Cargo.toml')),lock=path.resolve(cargo.lock||path.join(path.dirname(manifest),'Cargo.lock')),lockBytes=fs.readFileSync(lock),locked=lockedPackages(lockBytes.toString());
 if(cargo.lockSha256&&sha(lockBytes)!==cargo.lockSha256)throw Error('Cargo lock notice identity differs');
 const features=cargo.features||['crabbyavif/android_mediacodec','crabbyavif/capi','crabbyavif/heic','crabbyavif/libyuv'];
 const meta=JSON.parse(execFileSync(ctx.tools.cargo,['metadata','--offline','--locked','--format-version','1','--filter-platform','aarch64-linux-android','--manifest-path',manifest,...(features.length?['--features',features.join(',')]:[])],{encoding:'utf8',maxBuffer:32<<20,env:{...process.env,CARGO_HOME:ctx.tools.cargoHome,CARGO_NET_OFFLINE:'true',...(ctx.tools.rustc?{RUSTC:ctx.tools.rustc}:{})}}));
 if(!fs.readFileSync(lock).equals(lockBytes))throw Error('Cargo lock changed while collecting notices');
 if(!meta.resolve?.nodes||!meta.workspace_members)throw Error('Offline Cargo dependency resolution is unavailable');
 const runtimePackages=runtimeCargoPackages(meta);
 const cache=path.resolve(cargo.registryCache||path.join(ctx.tools.cargoHome,'registry/cache')),cacheDirs=[cache,...fs.readdirSync(cache,{withFileTypes:true}).filter(d=>d.isDirectory()).map(d=>path.join(cache,d.name))];
 for(const pkg of runtimePackages.filter(p=>p.source?.startsWith('registry+'))){
  const checksum=locked.get([pkg.name,pkg.version,pkg.source].join('@'));if(!/^[a-f0-9]{64}$/.test(checksum||''))throw Error('Registry dependency has no exact Cargo lock checksum: '+pkg.name);
  if(!/^[a-zA-Z0-9_-]+$/.test(pkg.name)||!/^[a-zA-Z0-9_.+-]+$/.test(pkg.version))throw Error('Invalid registry dependency identity');const filename=pkg.name+'-'+pkg.version+'.crate',archive=cacheDirs.map(dir=>path.join(dir,filename)).find(file=>fs.existsSync(file)&&sha(fs.readFileSync(file))===checksum);if(!archive)throw Error('Authenticated offline crate archive is unavailable: '+filename);
  const files=tarFiles(fs.readFileSync(archive)),prefix=pkg.name+'-'+pkg.version+'/',names=[...files.keys()].filter(file=>file.startsWith(prefix)&&!file.slice(prefix.length).includes('/')&&/^(LICENSE|LICENCE|COPYING|COPYRIGHT|NOTICE|PATENTS)([._-]|$)/i.test(file.slice(prefix.length)));
  if(pkg.license_file){const file=prefix+relative(pkg.license_file);if(!names.includes(file))names.push(file);}if(!names.length)throw Error('Original registry dependency notice is absent: '+pkg.name);
  const noticeFiles=[];for(const name of names.sort()){const rel=name.slice(prefix.length);add('rust/'+prefix+rel,member(files,name));noticeFiles.push(rel);}registry.push({name:pkg.name,version:pkg.version,source:pkg.source,checksum,license:pkg.license,noticeFiles});
 }
 for(const[kind,input]of [['rust-standard-library',rust],['ndk',ndk]]){
  const root=path.resolve(input.root);let files=input.files;if(!files){if(kind==='rust-standard-library')files=rustLibraryNoticeFiles(root);else throw Error('Explicit original Android NDK notice file paths are required');}
  if(!files.length)throw Error('Original toolchain notice files are absent');for(const item of files){const record=typeof item==='string'?{path:item}:item,rel=relative(record.path),file=path.join(root,rel),bytes=fs.readFileSync(file);if(record.sha256&&sha(bytes)!==record.sha256)throw Error('Original toolchain notice identity differs: '+kind+'/'+rel);add(kind+'/'+(record.output||rel),bytes,{project:kind,version:input.version,licensePath:rel});}
 }
 add('SOURCES.json',Buffer.from(JSON.stringify({git:sources.map(s=>({project:s.project,repository:s.repository,revision:s.revision})),noticeOrigins:origins,registry,cargoLockSha256:sha(lockBytes)},null,2)+'\n'));
 let ancestor=output;while(!fs.existsSync(ancestor))ancestor=path.dirname(ancestor);const realWork=fs.realpathSync(ctx.work),physical=path.join(fs.realpathSync(ancestor),path.relative(ancestor,output));if(!physical.startsWith(realWork+path.sep))throw Error('HWUI notice output leaves the declared work directory');
 if(fs.existsSync(output)){const walk=dir=>{for(const entry of fs.readdirSync(dir,{withFileTypes:true})){const file=path.join(dir,entry.name),rel=path.relative(output,file).split(path.sep).join('/');if(entry.isDirectory())walk(file);else if(!entry.isFile()||!pending.has(rel))throw Error('Unexpected existing HWUI notice output: '+rel);}};walk(output);}
 for(const file of pending.keys())if(fs.existsSync(path.join(output,file))&&!fs.readFileSync(path.join(output,file)).equals(pending.get(file).bytes))throw Error('Existing notice output differs: '+file);
 fs.mkdirSync(output,{recursive:true});for(const[file,{bytes}]of pending){const destination=path.join(output,file);fs.mkdirSync(path.dirname(destination),{recursive:true});if(!fs.existsSync(destination))fs.writeFileSync(destination,bytes,{flag:'wx'});}
 return{root:output,records:[...pending.values()].map(v=>v.record).filter(record=>isLicenseNoticeFile(record.path)).sort((a,b)=>a.path.localeCompare(b.path))};
}
