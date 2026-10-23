import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {writeChanged} from './source-files.mjs';

export function exportSources(checkout,revision,directories,destination,{offline=false}={}){
  // Inventory object stores have an explicit release commit and need no
  // checked-out branch. Live source checkouts retain their exact HEAD check.
  const bare=execFileSync('git',['-C',checkout,'rev-parse','--is-bare-repository'],{encoding:'utf8',env:{...process.env,GIT_NO_LAZY_FETCH:'1'}}).trim()==='true';
  const actual=execFileSync('git',['-C',checkout,'rev-parse','--verify',bare?revision+'^{commit}':'HEAD'],{encoding:'utf8',env:{...process.env,GIT_NO_LAZY_FETCH:'1'}}).trim();
  if(actual!==revision)throw Error('Android source revision does not match source-pins.json: '+checkout);
  fs.mkdirSync(destination,{recursive:true});
  // git archive can prefetch unrelated blobs in partial clones. Read exactly
  // the selected source trees, preserving binary bytes and original paths.
  const files=execFileSync('git',['-C',checkout,'ls-tree','-r','-z',revision,'--',...directories],{encoding:'utf8',maxBuffer:32<<20,env:{...process.env,GIT_NO_LAZY_FETCH:'1'}}).split('\0').filter(Boolean).map(line=>{
    const at=line.indexOf('\t'),[mode,type,oid]=line.slice(0,at).split(' ');return {mode,type,oid,file:line.slice(at+1)};
  }).filter(file=>file.type==='blob');
  const manifest=path.join(destination,'.maclauncher-exported.json');
  const previous=fs.existsSync(manifest)?JSON.parse(fs.readFileSync(manifest,'utf8')):[];
  if(!Array.isArray(previous)||previous.some(file=>typeof file!=='string'||!file||path.isAbsolute(file)||file.split('/').includes('..')))throw Error('Invalid original source export index: '+manifest);
  const selected=file=>directories.some(directory=>directory==='.'||file===directory||file.startsWith(directory.replace(/\/$/,'')+'/'));
  const current=new Set(files.map(file=>file.file));
  for(const file of previous)if(selected(file)&&!current.has(file))fs.rmSync(path.join(destination,file),{recursive:true,force:true});
  const retained=[...new Set([...previous.filter(file=>!selected(file)),...current])].sort();
  if(!files.length){writeChanged(manifest,JSON.stringify(retained)+'\n');return;}
  // Partial clones otherwise fetch once per blob while cat-file consumes the
  // stream. Fetch this exact source closure together, without unrelated trees.
  const objects=[...new Set(files.map(file=>file.oid))];
  const states=execFileSync('git',['-C',checkout,'cat-file','--batch-check'],{
    input:objects.join('\n')+'\n',encoding:'utf8',env:{...process.env,GIT_NO_LAZY_FETCH:'1'},maxBuffer:16<<20,
  }).trim().split('\n');
  const missing=states.filter(line=>line.endsWith(' missing')).map(line=>line.split(' ')[0]);
  if(missing.length&&offline)throw Error('Original source objects are unavailable locally: '+missing.length);
  if(missing.length)execFileSync('git',['-C',checkout,'fetch','--no-tags','--no-write-fetch-head','origin','--stdin'],{
    input:missing.join('\n')+'\n',stdio:['pipe','ignore','inherit'],
  });
  const blobs=execFileSync('git',['-C',checkout,'cat-file','--batch'],{input:files.map(({file})=>revision+':'+file).join('\n')+'\n',maxBuffer:128<<20,env:{...process.env,GIT_NO_LAZY_FETCH:'1'}});
  let offset=0;
  for(const {file,mode} of files){
    const end=blobs.indexOf(10,offset),header=blobs.subarray(offset,end).toString(),size=Number(header.split(' ')[2]);
    if(end<0||!Number.isSafeInteger(size)||!header.includes(' blob '))throw Error('Android source blob is unavailable: '+file);
    const target=path.join(destination,file);fs.mkdirSync(path.dirname(target),{recursive:true});
    const bytes=blobs.subarray(end+1,end+1+size);
    if(mode==='120000'){
      const old=fs.lstatSync(target,{throwIfNoEntry:false}),link=bytes.toString();
      if(!old?.isSymbolicLink()||fs.readlinkSync(target)!==link){if(old)fs.rmSync(target,{recursive:true,force:true});fs.symlinkSync(link,target);}
    }else writeChanged(target,bytes,mode==='100755'?0o755:0o644);
    offset=end+size+2;
  }
  writeChanged(manifest,JSON.stringify(retained)+'\n');
}
