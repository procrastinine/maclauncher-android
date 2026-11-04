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

/** Record the original Git and locked registry source closure for source archives. */
export async function recordHwuiSources(ctx,{cargo={}}={}){
 const sources=Object.values(ctx.originalSources||{}).map(value=>value.record),registry=[];
 let lockBytes=Buffer.alloc(0);
 {
  const manifest=path.resolve(cargo.manifest||path.join(ctx.components,'crabbyavif/rust/Cargo.toml')),lock=path.resolve(cargo.lock||path.join(path.dirname(manifest),'Cargo.lock'));lockBytes=fs.readFileSync(lock);
  const checksums=new Map(lockBytes.toString().split(/^\[\[package\]\]\s*$/m).slice(1).map(block=>{const get=name=>JSON.parse(block.match(new RegExp('^'+name+' = ("[^"]*")$','m'))?.[1]||'null');return [[get('name'),get('version'),get('source')].join('@'),get('checksum')];}));
  const features=cargo.features||['crabbyavif/android_mediacodec','crabbyavif/capi','crabbyavif/heic','crabbyavif/libyuv'];
  const meta=JSON.parse(execFileSync(ctx.tools.cargo,['metadata','--offline','--locked','--format-version','1','--filter-platform','aarch64-linux-android','--manifest-path',manifest,...(features.length?['--features',features.join(',')]:[])],{encoding:'utf8',maxBuffer:32<<20,env:{...process.env,CARGO_HOME:ctx.tools.cargoHome,CARGO_NET_OFFLINE:'true',...(ctx.tools.rustc?{RUSTC:ctx.tools.rustc}:{})}}));
  for(const pkg of runtimeCargoPackages(meta).filter(pkg=>pkg.source?.startsWith('registry+'))){
   registry.push({name:pkg.name,version:pkg.version,source:pkg.source,checksum:checksums.get([pkg.name,pkg.version,pkg.source].join('@'))});
  }
 }
 const sourceRecords=path.join(ctx.module||ctx.work,'sources');fs.mkdirSync(sourceRecords,{recursive:true});
 fs.writeFileSync(path.join(sourceRecords,'SOURCES.json'),JSON.stringify({git:sources.map(source=>({project:source.project,repository:source.repository,revision:source.revision})),registry,cargoLockSha256:crypto.createHash('sha256').update(lockBytes).digest('hex')},null,2)+'\n');
 return sourceRecords;
}
