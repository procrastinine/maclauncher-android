import {copyOptional} from '../upstream/source-files.mjs';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';

/** Cargo build dependencies and procedural macros execute on the build host. */
export function runtimeCargoPackages(meta){
 const nodes=new Map(meta.resolve.nodes.map(node=>[node.id,node])),packages=new Map(meta.packages.map(pkg=>[pkg.id,pkg])),active=new Set();
 function visit(id){
  if(active.has(id)||packages.get(id)?.targets?.some(target=>target.kind?.includes('proc-macro')))return;
  const node=nodes.get(id);if(!node)return;active.add(id);
  for(const dep of node.deps||[])if(dep.dep_kinds.some(kind=>kind.kind===null))visit(dep.pkg);
 }
 for(const id of meta.workspace_members)visit(id);return meta.packages.filter(pkg=>active.has(pkg.id));
}

/** Accompany the compiled product with candidates; Builds sealing judges them. */
export async function collectHwuiNotices(ctx,{output,cargo={},rust,ndk,sourceFiles}={}){
 output=path.resolve(output||path.join(ctx.module||ctx.work||process.cwd(),'notices'));
 const records=[],sources=Object.values(ctx.originalSources||{}).map(value=>value.record),registry=[];
 const copy=(file,relative)=>{copyOptional(file,path.join(output,relative));records.push({path:relative});};
 for(const record of sources)for(const check of record.noticeChecks||[]){
  const selected=sourceFiles?.[record.key]||ctx.sources?.[record.key]||ctx.originalSources[record.key]?.input;
  if(typeof selected==='string')copy(path.join(selected,check.path),check.output);
 }
 let lockBytes=Buffer.alloc(0);
 try{
  const manifest=path.resolve(cargo.manifest||path.join(ctx.components,'crabbyavif/rust/Cargo.toml')),lock=path.resolve(cargo.lock||path.join(path.dirname(manifest),'Cargo.lock'));lockBytes=fs.readFileSync(lock);
  const checksums=new Map(lockBytes.toString().split(/^\[\[package\]\]\s*$/m).slice(1).map(block=>{const get=name=>JSON.parse(block.match(new RegExp('^'+name+' = ("[^"]*")$','m'))?.[1]||'null');return [[get('name'),get('version'),get('source')].join('@'),get('checksum')];}));
  const features=cargo.features||['crabbyavif/android_mediacodec','crabbyavif/capi','crabbyavif/heic','crabbyavif/libyuv'];
  const meta=JSON.parse(execFileSync(ctx.tools.cargo,['metadata','--offline','--locked','--format-version','1','--filter-platform','aarch64-linux-android','--manifest-path',manifest,...(features.length?['--features',features.join(',')]:[])],{encoding:'utf8',maxBuffer:32<<20,env:{...process.env,CARGO_HOME:ctx.tools.cargoHome,CARGO_NET_OFFLINE:'true',...(ctx.tools.rustc?{RUSTC:ctx.tools.rustc}:{})}}));
  for(const pkg of runtimeCargoPackages(meta).filter(pkg=>pkg.source?.startsWith('registry+'))){
   const root=path.dirname(pkg.manifest_path),prefix=pkg.name+'-'+pkg.version,files=new Set(fs.readdirSync(root).filter(name=>/^(LICENSE|LICENCE|COPYING|COPYRIGHT|NOTICE|PATENTS)([._-]|$)/i.test(name)));
   if(pkg.license_file)files.add(pkg.license_file);
   for(const file of files)copy(path.join(root,file),'rust/'+prefix+'/'+file);
   registry.push({name:pkg.name,version:pkg.version,source:pkg.source,checksum:checksums.get([pkg.name,pkg.version,pkg.source].join('@')),license:pkg.license,noticeFiles:[...files]});
  }
 }catch(error){console.warn('Skipped optional Cargo accompanying files: '+error.message);}
 for(const [kind,input]of [['rust-standard-library',rust],['ndk',ndk]]){
  if(!input?.root)continue;
  if(input.files)for(const item of input.files){const record=typeof item==='string'?{path:item}:item;copy(path.join(input.root,record.path),kind+'/'+(record.output||record.path));}
  else{copyOptional(path.join(input.root,'COPYRIGHT-library.html'),path.join(output,kind,'COPYRIGHT-library.html'));copyOptional(path.join(input.root,'licenses'),path.join(output,kind,'licenses'));}
 }
 const sourceRecords=path.join(ctx.module||ctx.work,'sources');fs.mkdirSync(sourceRecords,{recursive:true});
 fs.writeFileSync(path.join(sourceRecords,'SOURCES.json'),JSON.stringify({git:sources.map(source=>({project:source.project,repository:source.repository,revision:source.revision})),registry,cargoLockSha256:crypto.createHash('sha256').update(lockBytes).digest('hex')},null,2)+'\n');
 return {root:output,records,sourceRecords};
}
