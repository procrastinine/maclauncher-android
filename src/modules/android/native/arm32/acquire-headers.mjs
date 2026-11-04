import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import zlib from 'node:zlib';
import {execFileSync} from 'node:child_process';

// Explicit developer acquisition. Original headers, ARM compiler builtins,
// link-only NDK libraries are transferred; no NDK compiler is needed.
export function acquireNdkHeaders(output,{linkLibrariesOnly=false}={}){
  const pin=JSON.parse(fs.readFileSync(new URL('./ndk-headers.json',import.meta.url)));
  const links=new Map(pin.linkLibraries.map(row=>[row.entry,row]));
  const linkDirectory=path.resolve(output,'link-libraries');
  const valid=(file,row)=>fs.existsSync(file)&&fs.statSync(file).size===row.bytes&&crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex')===row.sha256;
  if(linkLibrariesOnly&&pin.linkLibraries.every(row=>valid(path.join(linkDirectory,row.file),row)))return {linkLibraries:linkDirectory};
  const range=(start,end)=>execFileSync('curl',['--fail','--silent','--show-error','--retry','2','--range',`${start}-${end}`,pin.url+'?maclauncher-ndk-headers=1'],{maxBuffer:128*1024*1024});
  const tail=range(pin.archiveBytes-65536,pin.archiveBytes-1);
  const end=tail.lastIndexOf(Buffer.from([0x50,0x4b,0x05,0x06]));
  if(end<0)throw Error('NDK ZIP directory was not found');
  const centralSize=tail.readUInt32LE(end+12),centralOffset=tail.readUInt32LE(end+16);
  const central=range(centralOffset,centralOffset+centralSize-1);
  if(crypto.createHash('sha256').update(central).digest('hex')!==pin.centralDirectorySha256)throw Error('NDK ZIP directory differs from the pinned release');
  const entries=[];
  for(let offset=0;offset<central.length;){
    if(central.readUInt32LE(offset)!==0x02014b50)throw Error('Invalid NDK ZIP directory');
    const names=central.readUInt16LE(offset+28),extra=central.readUInt16LE(offset+30),comment=central.readUInt16LE(offset+32);
    const name=central.toString('utf8',offset+46,offset+46+names);
    if(links.has(name)||!linkLibrariesOnly&&((name.includes('/sysroot/usr/include/')&&!name.endsWith('/'))||new RegExp(`android-ndk-${pin.release}/source.properties$`).test(name)||name===pin.compilerRuntime.entry)){
      entries.push({name,method:central.readUInt16LE(offset+10),crc:central.readUInt32LE(offset+16),compressed:central.readUInt32LE(offset+20),size:central.readUInt32LE(offset+24),offset:central.readUInt32LE(offset+42)});
    }
    offset+=46+names+extra+comment;
  }
  const groups=[];
  for(const entry of entries.sort((a,b)=>a.offset-b.offset)){
    let group=groups.at(-1);
    if(!group||entry.offset>group.end+65536){group={start:entry.offset,end:entry.offset,entries:[]};groups.push(group);}
    group.entries.push(entry);group.end=entry.offset+30+Buffer.byteLength(entry.name)+65536+entry.compressed;
  }
  const special=new Map(pin.caseSensitiveHeaders.map(row=>[row.path,row.sha256]));
  const records=[];
  for(const group of groups){
    const data=range(group.start,Math.min(pin.archiveBytes-1,group.end));
    for(const entry of group.entries){
      const at=entry.offset-group.start;
      if(data.readUInt32LE(at)!==0x04034b50)throw Error('Invalid NDK ZIP entry');
      const start=at+30+data.readUInt16LE(at+26)+data.readUInt16LE(at+28),compressed=data.subarray(start,start+entry.compressed);
      const bytes=entry.method===0?compressed:zlib.inflateRawSync(compressed);
      if(bytes.length!==entry.size||zlib.crc32(bytes)!==entry.crc)throw Error('NDK ZIP integrity mismatch: '+entry.name);
      const relative=links.has(entry.name)?'link-libraries/'+links.get(entry.name).file:entry.name.includes('/sysroot/usr/include/')?'include/'+entry.name.split('/sysroot/usr/include/')[1]:path.basename(entry.name);
      if(relative.split('/').includes('..'))throw Error('Invalid NDK archive path');
      const sha256=crypto.createHash('sha256').update(bytes).digest('hex');
      const destination=path.join(output,relative);fs.mkdirSync(path.dirname(destination),{recursive:true});fs.writeFileSync(destination,bytes);
      const expected=special.get(relative.slice('include/'.length));
      if(expected){
        if(sha256!==expected)throw Error('NDK case-sensitive header differs from the pinned release');
        const alternate=path.join(output,'case-sensitive',sha256);fs.mkdirSync(path.dirname(alternate),{recursive:true});fs.writeFileSync(alternate,bytes);
      }
      records.push({path:relative,size:bytes.length,sha256});
    }
  }
  for(const row of pin.linkLibraries)if(!valid(path.join(linkDirectory,row.file),row))throw Error('NDK link library differs from the pinned release: '+row.file);
  if(linkLibrariesOnly)return {linkLibraries:linkDirectory};
  const headers=records.filter(row=>row.path.startsWith('include/')).map(row=>({...row,path:row.path.slice(8)})).sort((a,b)=>a.path<b.path?-1:a.path>b.path?1:0);
  const identity=crypto.createHash('sha256');for(const row of headers)identity.update(`${row.path}\0${row.size}\0${row.sha256}\n`);
  if(headers.length!==pin.files||identity.digest('hex')!==pin.treeSha256)throw Error('Downloaded NDK headers differ from the pinned release');
  for(const [name,hash]of[['source.properties',pin.sourcePropertiesSha256]])if(records.find(row=>row.path===name)?.sha256!==hash)throw Error('NDK metadata differs from the pinned release: '+name);
  for(const [name,hash]of[[pin.compilerRuntime.file,pin.compilerRuntime.sha256]])if(records.find(row=>row.path===name)?.sha256!==hash)throw Error('NDK compiler runtime differs from the pinned release: '+name);
  return {
    include:path.resolve(output,'include'),
    compilerRuntime:path.resolve(output,pin.compilerRuntime.file),
    linkLibraries:linkDirectory
  };
}

if(process.argv[1]&&path.resolve(process.argv[1])===new URL(import.meta.url).pathname){
  if(process.argv.length!==3)throw Error('Expected an output directory');
  console.log(JSON.stringify(acquireNdkHeaders(path.resolve(process.argv[2]))));
}
