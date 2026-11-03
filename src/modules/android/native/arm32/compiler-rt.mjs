import {addLicense} from '../upstream/licenses.mjs';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';

// Compiler-generated ARM ABI routines remain original ARM code. The common
// image loader executes them through the CPU adapter; their libc calls reach
// the same native owners as every other guest library.
export function buildCompilerRuntime({archive,notice,linker,output,aeabi,include,allocation,portableFamilies=[]}){
  const ndk=JSON.parse(fs.readFileSync(new URL('./ndk-headers.json',import.meta.url))),pin=ndk.compilerRuntime;
  const input=fs.readFileSync(archive),sha=b=>crypto.createHash('sha256').update(b).digest('hex');
  if(input.length!==pin.bytes||sha(input)!==pin.sha256)throw Error('ARM compiler runtime differs from the pinned NDK artifact');
  const bytes=Buffer.from(input);if(bytes.toString('ascii',0,8)!=='!<arch>\n')throw Error('Invalid compiler runtime archive');
  let exported=0;
  for(let at=8;at<bytes.length;){
    if(bytes.toString('ascii',at+58,at+60)!=='`\n')throw Error('Invalid compiler runtime archive member');
    const size=Number(bytes.toString('ascii',at+48,at+58).trim()),start=at+60;
    if(start+size>bytes.length)throw Error('Truncated compiler runtime archive');
    if(bytes.toString('ascii',start,start+4)==='\x7fELF'){
      if(bytes[start+4]!==1||bytes[start+5]!==1)throw Error('Expected original little-endian ELF32 compiler routines');
      const sections=start+bytes.readUInt32LE(start+32),stride=bytes.readUInt16LE(start+46),count=bytes.readUInt16LE(start+48);
      for(let n=0;n<count;n++){
        const section=sections+n*stride;if(bytes.readUInt32LE(section+4)!==2)continue;
        const symbols=start+bytes.readUInt32LE(section+16),length=bytes.readUInt32LE(section+20),entry=bytes.readUInt32LE(section+36);
        for(let symbol=symbols;symbol<symbols+length;symbol+=entry){
          const binding=bytes[symbol+12]>>4;
          // Archive globals are linkable by consumers. Publish that same set
          // from the shared ELF instead of letting hidden static visibility
          // discard it. Code, data, aliases and weak binding remain unchanged.
          if((binding===1||binding===2)&&bytes.readUInt16LE(symbol+14)){bytes[symbol+13]&=~3;exported++;}
        }
      }
    }
    at=start+size+(size&1);
  }
  fs.mkdirSync(output,{recursive:true});
  const prepared=path.join(output,'compiler-rt-exports.a'),library=path.join(output,'libandroid-arm32-builtins.so');fs.writeFileSync(prepared,bytes);
  // Bionic's whole ARM ABI unit supplies lifecycle/memory adapters absent from
  // compiler-rt. Its original wrappers retain the common libc service owners.
  const objects=[],versions=[];
  if(aeabi){
    const object=path.join(output,'bionic-aeabi.o');
    execFileSync('clang',['--target=armv7a-linux-androideabi21','-isystem',include,'-isystem',path.join(include,'arm-linux-androideabi'),'-fPIC','-fno-builtin','-O2','-c',aeabi,'-o',object],{stdio:'inherit'});
    objects.push(object);
    const names=[...new Set([...fs.readFileSync(aeabi,'utf8').matchAll(/@@?([A-Z][A-Z0-9_]*)/g)].map(match=>match[1]))];
    const versionFile=path.join(output,'bionic-aeabi.map');fs.writeFileSync(versionFile,names.map(name=>name+' {};').join('\n'));
    versions.push('--version-script='+versionFile);
  }
  if(allocation){
    const object=path.join(output,'bionic-new.o');
    execFileSync('clang++',['--target=armv7a-linux-androideabi21','-std=c++17','-nostdinc++','-I',allocation.include,'-isystem',include,'-isystem',path.join(include,'arm-linux-androideabi'),'-fPIC','-fno-exceptions','-fno-rtti','-fno-builtin','-O2','-c',allocation.source,'-o',object],{stdio:'inherit'});
    objects.push(object);
  }
  // CPU register state, C++ size_t and pointer-bearing search structures retain
  // their original guest-width implementations. Their libc dependencies use
  // exactly the same native import owners as every other ARM32 image.
  for(const [familyIndex,family] of portableFamilies.entries())for(const source of family.units){
    const object=path.join(output,'family-'+familyIndex+'-'+path.basename(source)+'.o'),cpp=source.endsWith('.cpp');
    execFileSync(cpp?'clang++':'clang',['--target=armv7a-linux-androideabi'+(family.api||21),'-mfloat-abi=softfp','-mfpu=neon','-isystem',include,'-isystem',path.join(include,'arm-linux-androideabi'),'-I',family.include,...(family.includes||[]).flatMap(directory=>['-I',directory]),...(source.endsWith('.S')?['-D__ASSEMBLY__']:[]),...(family.annotations?['-include',family.annotations]:[]),...(cpp?['-std=c++17','-nostdinc++','-fno-exceptions','-fno-rtti']:[]),'-fPIC','-fno-builtin','-O2','-c',source,'-o',object],{stdio:'inherit'});
    objects.push(object);
  }
  // Retain real ELF dependencies for the native endpoints used by portable
  // families. These original NDK files are link metadata only; each runtime
  // resolves the SONAME to its existing native implementation and namespace.
  const dependencies=[...new Set(portableFamilies.flatMap(family=>family.libraries||[]))].map(name=>{
    const record=ndk.linkLibraries.find(row=>row.file===name),file=path.resolve(include,'../link-libraries',name);
    if(!record)throw Error('Original NDK link library is not declared: '+name);
    const bytes=fs.readFileSync(file);if(bytes.length!==record.bytes||sha(bytes)!==record.sha256)throw Error('NDK link library differs from the pinned release: '+name);
    return file;
  });
  execFileSync(linker,['-flavor','gnu','-shared','--hash-style=both','--soname=libandroid-arm32-builtins.so',...objects,...versions,'--whole-archive',prepared,'--no-whole-archive','--no-as-needed',...dependencies,'-o',library],{stdio:'inherit'});
  const licenses=path.join(output,'licenses');addLicense(path.dirname(licenses),'compiler-rt',notice);
  for(const family of portableFamilies)for(const file of family.licenses||[])addLicense(path.dirname(licenses),'compiler-rt',file);
  return {library,licenses,exported};
}
