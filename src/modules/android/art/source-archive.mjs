import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';
import {snapshotSources,snapshotBuildCompanions} from '../native/build-sources.mjs';
import Content from '../../shared/runtime/runtime-content.js';
import {exportHwuiSources} from './source-hwui.mjs';
import {exportSources} from '../native/upstream/source-export.mjs';

const repository=path.resolve(import.meta.dirname,'../../../..');
const sha=bytes=>crypto.createHash('sha256').update(bytes).digest('hex');
const git=(directory,args)=>execFileSync('git',['-C',directory,...args],{encoding:'utf8',maxBuffer:32<<20,env:{...process.env,GIT_NO_LAZY_FETCH:'1'},stdio:['ignore','pipe','pipe']}).trim();
const relative=value=>typeof value==='string'&&!path.isAbsolute(value)&&!value.split(/[\\/]/).some(part=>part==='..'||part==='.')&&!value.includes('\\');
function copySource(from,to,logical=''){
 if(Content.excludedReason(logical)||path.basename(from)==='.fetched')return;
 const stat=fs.lstatSync(from);
 if(stat.isDirectory())for(const name of fs.readdirSync(from).sort())copySource(path.join(from,name),path.join(to,name),path.posix.join(logical,name));
 else if(stat.isFile()){fs.mkdirSync(path.dirname(to),{recursive:true});fs.copyFileSync(from,to);fs.chmodSync(to,stat.mode&0o111?0o755:0o644);}
 else if(stat.isSymbolicLink()){const target=fs.readlinkSync(from);if(path.isAbsolute(target))throw Error('ART source aliases require relative targets: '+logical);fs.mkdirSync(path.dirname(to),{recursive:true});fs.symlinkSync(target,to);}
 else throw Error('ART source inputs require regular files: '+logical);
}
function archiveCheckout(record,checkout,output,trees=['.']){
 if(typeof checkout!=='string'||!path.isAbsolute(checkout))throw Error('ART source export requires the '+record.id+' source checkout.');
 if(git(checkout,['rev-parse','HEAD'])!==record.revision)throw Error('ART source checkout differs from its selected revision: '+record.id);
 const selected=[...new Set([...trees,...record.licensePaths])];
 for(const name of record.licensePaths)git(checkout,['cat-file','-e',record.revision+':'+name]);
 const members=git(checkout,['ls-tree','-r','--name-only','-z',record.revision,'--',...selected]).split('\0').filter(Boolean);
 const published=members.filter(name=>!Content.excludedReason(name));
 const temporary=fs.mkdtempSync(path.join(path.dirname(output),'.pinned-source-'));
 try{
  exportSources(checkout,record.revision,published,temporary,{offline:true});fs.rmSync(path.join(temporary,'.maclauncher-exported.json'));
  normalizeSource(temporary);execFileSync('/usr/bin/tar',['--uid','0','--gid','0','--uname','root','--gname','wheel','-cf',output,'-C',temporary,...fs.readdirSync(temporary).sort()],{env:{...process.env,COPYFILE_DISABLE:'1'},stdio:['ignore','ignore','pipe']});
 }finally{fs.rmSync(temporary,{recursive:true,force:true});}
}
function normalizeSource(directory){for(const entry of fs.readdirSync(directory,{withFileTypes:true})){const file=path.join(directory,entry.name);if(entry.isDirectory())normalizeSource(file);if(entry.isSymbolicLink())fs.lutimesSync(file,0,0);else fs.utimesSync(file,0,0);}fs.utimesSync(directory,0,0);}
function lockedArtSources(aim,aosp,destination){
 if(typeof aosp!=='string'||!path.isAbsolute(aosp)||!fs.existsSync(aosp))throw Error('Default ART source export requires sources.aosp with the original AOSP inputs from patches/art-android/sources.lock, or an explicit sourceArchive.');
 const text=git(aim,['show','HEAD:patches/art-android/sources.lock']),tag=text.match(/^AOSP_TAG=([^\s#]+)/m)?.[1],records=[];
 if(!tag)throw Error('ART source lock has no AOSP tag.');
 function regularFiles(directory){const result=[];function walk(at,relative=''){for(const item of fs.readdirSync(at,{withFileTypes:true})){if(item.name==='.fetched')continue;const name=path.posix.join(relative,item.name),file=path.join(at,item.name);if(item.isDirectory())walk(file,name);else if(item.isFile())result.push(name);else if(!item.isSymbolicLink())throw Error('ART AOSP source must be a regular file: '+name);}}walk(directory);return result.sort((a,b)=>Buffer.compare(Buffer.from(a),Buffer.from(b)));}
 for(const [section,tree]of [['SOURCE_TREES',true],['SOURCE_FILES',false]]){
  const block=text.match(new RegExp('^'+section+'=\\(\\n([\\s\\S]*?)^\\)','m'))?.[1];if(block===undefined)throw Error('ART source lock is missing '+section+'.');
  for(const match of block.matchAll(/^\s*"([^"]+)"/gm)){
   const [project,subtree,digest,revision]=match[1].split('|');
   if(!relative(project)||subtree&&!relative(subtree)||!/^[a-f0-9]{64}$/.test(digest))throw Error('ART source lock contains an invalid source input.');
   const name=path.posix.join(project.replace(/^platform\//,''),subtree),file=path.join(aosp,name);
   if(!fs.existsSync(file))throw Error('ART source export is missing original AOSP input: '+name);
   const actual=tree?sha(regularFiles(file).map(name=>sha(fs.readFileSync(path.join(file,name)))+'  ./'+name+'\n').join('')):sha(fs.readFileSync(file));
   if(actual!==digest)throw Error('ART original AOSP source differs from its source lock: '+name);
   copySource(file,path.join(destination,name),name);
   records.push({project,path:subtree,revision:revision||'refs/tags/'+tag,sha256:digest});
  }
 }
 return records;
}

function nativeBridgeTrees({arm32Notices},revision){
 if(!path.isAbsolute(arm32Notices||''))throw Error('Native bridge source export requires its ARM32 component notices.');
 const read=name=>{const record=JSON.parse(fs.readFileSync(path.join(arm32Notices,name)));if(record.revision!==revision)throw Error('Native bridge source notice revision differs.');return record;};
 const elf=read('bionic.json'),families=read('bionic-portable-families.json');
 const files=[...elf.files,...Object.values(families).filter(Array.isArray).flat(),
 'linker/linker_namespaces.cpp','linker/linker_utils.cpp','libc/NOTICE','libc/include/search.h','libc/upstream-freebsd/lib/libc/stdlib/hsearch.h',
 'libc/stdio/printf_common.h','libc/stdio/scanf_common.h','libc/stdio/vfscanf.cpp','libc/SYSCALLS.TXT','libc/tools/gensyscalls.py',
 ...['prctl.h','capability.h','securebits.h','netlink.h','fcntl.h','binfmts.h'].map(name=>'libc/kernel/uapi/linux/'+name)];
 if(files.some(name=>!relative(name)))throw Error('Native bridge source notices contain an invalid source path.');return [...new Set(files)].sort();
}

function exportNativeBridgeHeaders({ndkHeaders},destination){
 if(!path.isAbsolute(ndkHeaders||''))throw Error('Native bridge source export requires an absolute ndkHeaders directory.');
 const pin=JSON.parse(fs.readFileSync(new URL('../native/arm32/ndk-headers.json',import.meta.url))),special=new Map(pin.caseSensitiveHeaders.map(row=>[row.path,row])),folded=new Set([...special.keys()].map(name=>name.toLowerCase())),files=[];
 function walk(at,rel=''){for(const item of fs.readdirSync(at,{withFileTypes:true})){const name=path.posix.join(rel,item.name);if(item.isDirectory())walk(path.join(at,item.name),name);else if(item.isFile()&&!folded.has(name.toLowerCase()))files.push(name);else if(!item.isFile())throw Error('NDK headers require regular source files.');}}walk(ndkHeaders);files.push(...special.keys());files.sort();
 const identity=crypto.createHash('sha256');
 for(const name of files){let file=path.join(ndkHeaders,name),bytes=fs.readFileSync(file),digest=sha(bytes);if(special.has(name)&&digest!==special.get(name).sha256){file=path.resolve(ndkHeaders,'../case-sensitive',special.get(name).sha256);bytes=fs.readFileSync(file);digest=sha(bytes);}identity.update(name+'\0'+bytes.length+'\0'+digest+'\n');}
 if(files.length!==pin.files||identity.digest('hex')!==pin.treeSha256)throw Error('Native bridge NDK headers differ from the pinned release.');
 copySource(ndkHeaders,path.join(destination,'include'));
 for(const row of pin.caseSensitiveHeaders){const input=path.resolve(ndkHeaders,'../case-sensitive',row.sha256);if(sha(fs.readFileSync(input))!==row.sha256)throw Error('Original case-sensitive NDK header is absent.');copySource(input,path.join(destination,'case-sensitive',row.sha256));}
 for(const [name,digest]of [['NOTICE',pin.noticeSha256],['source.properties',pin.sourcePropertiesSha256],['NOTICE.toolchain',pin.compilerRuntime.noticeSha256]]){const input=path.resolve(ndkHeaders,'..',name);if(sha(fs.readFileSync(input))!==digest)throw Error('Original NDK notice differs: '+name);copySource(input,path.join(destination,name));}
 fs.writeFileSync(path.join(destination,'source.json'),JSON.stringify(pin,null,2)+'\n');return {version:pin.version,url:pin.url,headerTreeSha256:pin.treeSha256,compilerRuntime:pin.compilerRuntime};
}

/** Export only owned producers, pinned source inputs and original ART source
 * subsets. Build directories, application data and prior payloads are not inputs. */
export function exportArtSourceArchive({output,sources,unicorn,sourceCheckouts={},nativeBridgeSources,hwuiSources}){
 if(!path.isAbsolute(output||''))throw Error('ART source archive requires an absolute output path.');
 if(!path.isAbsolute(sources?.aosp||'')||!fs.existsSync(sources.aosp))throw Error('Default ART source export requires sources.aosp with the original locked AOSP inputs, or an explicit sourceArchive.');
 const previous=fs.lstatSync(output,{throwIfNoEntry:false});
 if(previous&&!previous.isFile())throw Error('ART source archive output must be a regular generated file.');
 const projects=JSON.parse(fs.readFileSync(path.join(repository,'upstreams.json'))).projects;
 const defaults={aim:sources?.source,klepton:sources?.klepton,'android-gles-headers':sources?.glesHeaders,unicorn:unicorn||path.join(repository,'src/external/unicorn'),'aosp-native':sourceCheckouts['aosp-native']||path.join(repository,'src/external/aosp-native')};
 const selected={...sourceCheckouts,...defaults};
 if(nativeBridgeSources)for(const id of ['libffi','dynarmic','aosp-bionic','aosp-art'])if(!selected[id])throw Error('Native bridge source export requires sourceCheckouts.'+id+'.');
 for(const id of Object.keys(selected))if(!projects.some(project=>project.id===id))throw Error('ART source input is not registered: '+id);
 fs.mkdirSync(path.dirname(output),{recursive:true});const temporary=fs.mkdtempSync(path.join(path.dirname(output),'.art-sources-')),tree=path.join(temporary,'source'),snapshot=path.join(temporary,'snapshot'),archive=path.join(temporary,'source.tar.gz');
 try{
  fs.mkdirSync(tree);snapshotSources(path.join(repository,'src/modules/android/native'),snapshot);snapshotBuildCompanions({repository,destination:path.join(snapshot,'build-tools')});
  for(const [from,to]of [['native','src/modules/android/native'],['art','src/modules/android/art'],['profile.js','src/modules/android/profile.js'],['build-tools','']])copySource(path.join(snapshot,from),path.join(tree,to),to);
  for(const name of ['LICENSE','README.md','package.json','upstreams.json','.gitmodules','tools/build.mjs','tools/build-art.mjs','tools/check-sources.mjs'])copySource(path.join(repository,name),path.join(tree,name),name);
  const upstream=path.join(tree,'upstream');fs.mkdirSync(upstream);const records=[];
  for(const [id,checkout]of Object.entries(selected).sort(([a],[b])=>a.localeCompare(b))){
   const record=projects.find(project=>project.id===id),name=id+'.tar';
   const trees=id==='klepton'?['runtime']:id==='android-gles-headers'?['include','scripts/gl.xml']:id==='aosp-native'?['libs/binder']:id==='aosp-bionic'&&nativeBridgeSources?nativeBridgeTrees(nativeBridgeSources,record.revision):id==='aosp-art'?['libnativeloader/include/nativeloader/dlext_namespaces.h','libnativeloader/library_namespaces.cpp','libnativebridge/include/nativebridge/native_bridge.h']:['.'];
   archiveCheckout(record,checkout,path.join(upstream,name),trees);records.push({id,repository:record.repository,revision:record.revision,archive:'upstream/'+name,licensePaths:record.licensePaths});
  }
  const xsdcRevision=git(sources.source,['show','HEAD:upstream/android16-xsdc.lock']).match(/^XSDC_REVISION=([a-f0-9]{40})/m)?.[1];
  if(!xsdcRevision)throw Error('Original ART source has no pinned XSD compiler source.');
  const xsdc={id:'xsdc',repository:'https://android.googlesource.com/platform/system/tools/xsdc',revision:xsdcRevision,licensePaths:[]};
  archiveCheckout(xsdc,sources.xsdc||path.join(path.dirname(sources.aosp||''),'xsdc'),path.join(upstream,'xsdc.tar'));records.push({...xsdc,archive:'upstream/xsdc.tar'});
  const aosp=lockedArtSources(sources.source,sources.aosp,path.join(upstream,'aim-aosp'));
  const nativeBridge=nativeBridgeSources?exportNativeBridgeHeaders(nativeBridgeSources,path.join(upstream,'native-bridge-ndk')):undefined;
  const hwui=hwuiSources?exportHwuiSources(hwuiSources,path.join(upstream,'hwui')):undefined;
  fs.writeFileSync(path.join(tree,'source-inputs.json'),JSON.stringify({schemaVersion:1,projects:records,artSources:aosp,...(nativeBridge?{nativeBridge}:{}),...(hwui?{hwui}:{})},null,2)+'\n');
  function normalize(directory){for(const entry of fs.readdirSync(directory,{withFileTypes:true})){const file=path.join(directory,entry.name);if(entry.isDirectory())normalize(file);if(entry.isSymbolicLink())fs.lutimesSync(file,0,0);else fs.utimesSync(file,0,0);}fs.utimesSync(directory,0,0);}
  normalize(tree);
  execFileSync('/usr/bin/tar',['--uid','0','--gid','0','--uname','root','--gname','wheel','-czf',archive,'-C',tree,...fs.readdirSync(tree).sort()],{env:{...process.env,COPYFILE_DISABLE:'1'},stdio:['ignore','ignore','pipe']});
  const gzip=fs.readFileSync(archive);if(gzip.length<10||gzip[0]!==31||gzip[1]!==139||gzip[2]!==8)throw Error('ART source output is not a gzip archive.');gzip.writeUInt32LE(0,4);fs.writeFileSync(archive,gzip);
  if(!previous||sha(fs.readFileSync(output))!==sha(fs.readFileSync(archive)))fs.renameSync(archive,output);
  return output;
 }finally{fs.rmSync(temporary,{recursive:true,force:true});}
}
