import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';
import {verifyHwuiSource} from '../native/hwui/inputs.mjs';
import {exportSources} from '../native/upstream/source-export.mjs';
import Content from '../../shared/runtime/runtime-content.js';

const manifest=JSON.parse(fs.readFileSync(new URL('../native/hwui/sources.json',import.meta.url)));
const lockBytes=fs.readFileSync(new URL('../native/hwui/Cargo.lock',import.meta.url));
const sha=bytes=>crypto.createHash('sha256').update(bytes).digest('hex');
function relative(value){if(typeof value!=='string'||!value||path.isAbsolute(value)||value.split(/[\\/]/).some(part=>part==='..'||part==='.'||!part))throw Error('Invalid HWUI source member path.');return value;}
function copySource(from,to,logical){
 if(Content.excludedReason(logical)||['.git','.fetched','.clang-format','rustfmt.toml','.rustfmt.toml'].includes(path.basename(from)))return;
 const stat=fs.lstatSync(from);
 if(stat.isDirectory()){fs.mkdirSync(to,{recursive:true});for(const name of fs.readdirSync(from).sort())copySource(path.join(from,name),path.join(to,name),path.posix.join(logical,name));}
 else if(stat.isFile()){fs.mkdirSync(path.dirname(to),{recursive:true});fs.copyFileSync(from,to);fs.chmodSync(to,stat.mode&0o111?0o755:0o644);}
 else if(stat.isSymbolicLink()){const target=fs.readlinkSync(from);if(path.isAbsolute(target)||path.posix.normalize(path.posix.join(path.posix.dirname(logical),target)).startsWith('../'))throw Error('HWUI source alias leaves its source root: '+logical+' ('+(path.isAbsolute(target)?'absolute target':target)+').');fs.mkdirSync(path.dirname(to),{recursive:true});fs.symlinkSync(target,to);}
 else throw Error('HWUI source inputs require regular files.');
}
function original(verified,name,optional=false){
 for(const part of verified.roots){if(part.prefix&&name!==part.prefix&&!name.startsWith(part.prefix+'/'))continue;const tail=part.prefix?name.slice(part.prefix.length).replace(/^\//,''):name,file=path.join(part.root,tail);if(fs.existsSync(file)||fs.lstatSync(file,{throwIfNoEntry:false}))return file;}
 if(optional)return undefined;
 throw Error('Required original HWUI source is absent: '+verified.key+'/'+name);
}
function selection(record,verified){
 // buildAvif consumes the logger module; its adjacent rustfmt alias is a
 // formatter input in another AOSP project, outside this source projection.
 if(record.key==='logging')return record.checks.map(check=>check.path==='rust'?'rust/logger.rs':check.path);
 if(!record.checks.some(check=>check.path==='.'))return record.checks.map(check=>check.path);
 const includes=manifest.includes.flatMap(row=>row.source===record.key?[row.path]:row.work?.startsWith('module/source/'+record.key+'/')?[row.work.slice(('module/source/'+record.key+'/').length)]:[]);
 // These are the original consumers in buildStatics, buildAvif and
 // generateHeaders. Legacy framework use is limited to the catalog includes.
 const additional={hidl:['Android.bp','transport/base/1.0','transport/token/1.0','transport/manager/1.0'],frameworkLegacy:[],nativehelper:['Android.bp','libnativehelper_lazy.c','libnativehelper_lazy.h','libnativehelper.map.txt'],incfs:['incfs'],ziparchive:['Android.bp','incfs_support','zip_archive.cc','zip_archive_stream_entry.cc','zip_cd_entry_map.cc','zip_error.cpp','zip_writer.cc'],libhardware:[],boringssl:[],rustutils:['Android.bp','lib.rs','inherited_fd.rs','sockets.rs','users.rs','system_properties.rs','system_properties','bindgen']}[record.key];
 if(!additional)throw Error('HWUI whole-tree source has no declared production projection: '+record.key);
 if(record.key==='ziparchive')additional.push(...fs.readdirSync(verified.checks[0].file).filter(name=>/\.(?:h|hpp)$/.test(name)));
 return [...includes,...additional];
}
function normalize(root){for(const item of fs.readdirSync(root,{withFileTypes:true})){const file=path.join(root,item.name);if(item.isDirectory())normalize(file);if(item.isSymbolicLink())fs.lutimesSync(file,0,0);else fs.utimesSync(file,0,0);}fs.utimesSync(root,0,0);}
function lockedRegistry(){
 const result=new Map();for(const block of lockBytes.toString().split(/^\[\[package\]\]\s*$/m).slice(1)){const get=name=>{const value=block.match(new RegExp('^'+name+' = ("[^"\\n]*")$','m'))?.[1];return value?JSON.parse(value):undefined;};if(get('source')?.startsWith('registry+'))result.set([get('name'),get('version'),get('source')].join('@'),get('checksum'));}return result;
}
/** Export the original HWUI source projections and the selected, locked Rust
 * dependencies. Local source locations do not enter the returned identities. */
export function exportHwuiSources({sources,registry,registryCache}={},destination){
 if(!sources||!path.isAbsolute(destination||'')||!path.isAbsolute(registryCache||''))throw Error('HWUI source export requires explicit sources, registry cache and output directories.');
 if(typeof registry==='string')registry=JSON.parse(fs.readFileSync(registry)).registry;
 if(!Array.isArray(registry)||!registry.length)throw Error('HWUI source export requires the selected registry dependencies.');
 const previous=process.env.GIT_NO_LAZY_FETCH;process.env.GIT_NO_LAZY_FETCH='1';
 try{
  fs.mkdirSync(destination,{recursive:true});const projects=[];
  for(const record of manifest.sources){
   const verified=verifyHwuiSource(record,sources[record.key]);
   if(record.noticeChecks?.length)verifyHwuiSource({...record,checks:record.noticeChecks},sources[record.key]);
   const licenses=(record.licensePaths||[]).filter(name=>original(verified,name,true));
   const names=[...new Set([...selection(record,verified),...licenses,...(record.noticeChecks||[]).map(check=>check.path)])].map(relative).sort();
   const selected=names.filter(name=>!names.some(parent=>name!==parent&&name.startsWith(parent+'/')));
   let exported;
   try{for(const name of selected){
    let input=original(verified,name,true);
    if(!input){
     const checkout=verified.checks.find(check=>check.path==='.')?.file;
     if(!checkout)original(verified,name);
     // A clean sparse checkout authenticates the original tree but may not
     // materialize consumed headers. Export only those pinned local objects.
     if(!exported){
      let members=selected;
      if(record.key==='frameworkLegacy')members=execFileSync('git',['-C',checkout,'ls-tree','-r','--name-only','-z',record.revision,'--',...selected],{encoding:'utf8',env:{...process.env,GIT_NO_LAZY_FETCH:'1'}}).split('\0').filter(file=>file&&!Content.excludedReason(file)&&(selected.includes(file)||/\.(?:h|hpp|inc)$/.test(file))&&!file.split('/').includes('fuzz'));
      exported=fs.mkdtempSync(path.join(path.dirname(destination),'.hwui-source-'));exportSources(checkout,record.revision,members,exported,{offline:true});
     }
     input=path.join(exported,name);if(!fs.existsSync(input))original(verified,name);
    }
    copySource(input,path.join(destination,'sources',record.key,name),name);
   }}finally{if(exported)fs.rmSync(exported,{recursive:true,force:true});}
   projects.push({key:record.key,project:record.project,repository:record.repository,revision:record.revision,paths:selected,checks:record.checks});
  }
  const locked=lockedRegistry(),dependencies=[],seen=new Set(),cacheDirs=[registryCache,...fs.readdirSync(registryCache,{withFileTypes:true}).filter(item=>item.isDirectory()).map(item=>path.join(registryCache,item.name))];
  const temporary=fs.mkdtempSync(path.join(path.dirname(destination),'.hwui-registry-'));
  try{for(const record of [...registry].sort((a,b)=>(a.name+'@'+a.version).localeCompare(b.name+'@'+b.version))){
   if(!/^[a-zA-Z0-9_-]+$/.test(record.name||'')||!/^[a-zA-Z0-9_.+-]+$/.test(record.version||'')||!record.source?.startsWith('registry+'))throw Error('Invalid HWUI registry identity.');
   const identity=[record.name,record.version,record.source].join('@'),checksum=locked.get(identity);if(seen.has(identity))throw Error('Duplicate HWUI registry dependency.');seen.add(identity);
   if(!/^[a-f0-9]{64}$/.test(checksum||'')||record.checksum&&record.checksum!==checksum)throw Error('HWUI registry dependency differs from Cargo.lock: '+record.name);
   const prefix=record.name+'-'+record.version,filename=prefix+'.crate',archive=cacheDirs.map(dir=>path.join(dir,filename)).find(file=>fs.existsSync(file)&&sha(fs.readFileSync(file))===checksum);if(!archive)throw Error('Authenticated HWUI registry source is unavailable: '+filename);
   const names=execFileSync('/usr/bin/tar',['-tzf',archive],{encoding:'utf8',maxBuffer:16<<20}).split('\n').filter(Boolean);for(const name of names){const member=relative(name.replace(/\/$/,''));if(member!==prefix&&!member.startsWith(prefix+'/'))throw Error('HWUI registry archive has a different package root.');}
   const extracted=path.join(temporary,'original'),published=path.join(temporary,'published');fs.mkdirSync(extracted);fs.mkdirSync(published);
   execFileSync('/usr/bin/tar',['-xzf',archive,'-C',extracted],{env:{...process.env,COPYFILE_DISABLE:'1'},stdio:['ignore','ignore','pipe']});copySource(path.join(extracted,prefix),path.join(published,prefix),'');normalize(published);
   const logical='registry/'+prefix+'.tar.gz',output=path.join(destination,logical);fs.mkdirSync(path.dirname(output),{recursive:true});
   execFileSync('/usr/bin/tar',['--uid','0','--gid','0','--uname','root','--gname','wheel','-czf',output,'-C',published,prefix],{env:{...process.env,COPYFILE_DISABLE:'1'},stdio:['ignore','ignore','pipe']});const bytes=fs.readFileSync(output);
   dependencies.push({name:record.name,version:record.version,source:record.source,checksum,archive:logical,sha256:sha(bytes),size:bytes.length});fs.rmSync(extracted,{recursive:true});fs.rmSync(published,{recursive:true});
  }}finally{fs.rmSync(temporary,{recursive:true,force:true});}
  return {projects,registry:dependencies,cargoLockSha256:sha(lockBytes)};
 }finally{if(previous===undefined)delete process.env.GIT_NO_LAZY_FETCH;else process.env.GIT_NO_LAZY_FETCH=previous;}
}
