import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {exportSources} from '../binder-rpc/build.mjs';

const pins=JSON.parse(fs.readFileSync(new URL('../source-pins.json',import.meta.url)));

/** The original Bionic parser and target headers own syscall membership,
 * aliases, register layout and numbers for both guest execution widths. */
export function readSyscallStubs({bionicSource,python,output,architecture}){
  fs.mkdirSync(output,{recursive:true});
  const source=path.join(output,'source');
  exportSources(bionicSource,pins.aospBionic,['libc/SYSCALLS.TXT','libc/tools/gensyscalls.py','libc/NOTICE'],source);
  const rows=JSON.parse(execFileSync(python,[path.join(import.meta.dirname,'syscalls-parse.py'),
    path.join(source,'libc/tools/gensyscalls.py'),path.join(source,'libc/SYSCALLS.TXT'),architecture],{encoding:'utf8'}));
  return {rows,notice:path.join(source,'libc/NOTICE')};
}

export function syscallConstants(include,output,target,entries){
  const file=path.join(output,target+'-numbers.c');
  fs.writeFileSync(file,'#include <asm/unistd.h>\n#include <asm-generic/errno.h>\n#include <limits.h>\n'+
    entries.map(([name,value])=>`const unsigned ${name}=${value};`).join('\n')+'\n');
  const arch=target==='arm'?'arm-linux-androideabi':'aarch64-linux-android';
  const ir=execFileSync('clang',['--target='+arch+'36','-ffreestanding','-S','-emit-llvm',
    '-I'+include,'-I'+path.join(include,arch),file,'-o','-'],{encoding:'utf8'});
  const values=new Map([...ir.matchAll(/^@(\w+) = .*constant i32 (\d+)/gm)].map(m=>[m[1],Number(m[2])]));
  for(const [name] of entries)if(!values.has(name))throw Error('Original syscall constant was not emitted: '+name);
  return values;
}

/** Raw callers may use a kernel entry without a Bionic assembly stub (accept,
 * gettid and futex, for example). The target UAPI owns that full namespace. */
export function kernelSyscalls(include,output,target){
  const arch=target==='arm'?'arm-linux-androideabi':'aarch64-linux-android';
  const macros=execFileSync('clang',['--target='+arch+'36','-ffreestanding','-dM','-E','-x','c',
    '-I'+include,'-I'+path.join(include,arch),'-include','asm/unistd.h','/dev/null'],{encoding:'utf8'});
  const names=[...macros.matchAll(/^#define (__NR_(\w+))\s/gm)]
    .filter(([,macro])=>macro!=='__NR_syscalls');
  const numbers=syscallConstants(include,output,target,names.map(([,,name],i)=>['kernel'+i,'__NR_'+name]));
  return names.map(([,macro,name],i)=>({macro,name,number:numbers.get('kernel'+i)}));
}
