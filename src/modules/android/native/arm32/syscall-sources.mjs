import {addLicense} from '../upstream/licenses.mjs';
import {writeChanged} from '../upstream/source-files.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {exportSources} from '../binder-rpc/build.mjs';
import {preparePrctlConstants} from '../process-abi/prctl.mjs';

const pins=JSON.parse(fs.readFileSync(new URL('../source-pins.json',import.meta.url)));

// These are kernel/libc ABI differences, not a supported-function list. Every
// original ARM stub is inventoried below; ordinary matching declarations use
// the same generated import adapter. No Linux number is a Darwin syscall.
const adaptation={
  __brk:['','Unavailable'], // libc brk returns status; kernel brk returns the break.
  __exit:['','Unavailable'], // Kernel thread exit must not become libc process exit.
  __timer_create:['','Unavailable'], __timer_settime:['','Unavailable'],
  __timer_gettime:['','Unavailable'], __timer_getoverrun:['','Unavailable'],
  __timer_delete:['','Unavailable'], // Kernel timer IDs are not libc timer_t.
  __openat:['openat','OpenAt'], __ptrace:['ptrace','Ptrace'],
  prctl:['prctl','Direct'],
  __mmap2:['__mmap2','Direct'], __llseek:['lseek64','Seek'],
  __getpriority:['getpriority','Priority'], __getcwd:['getcwd','Getcwd'],
  __clock_nanosleep:['clock_nanosleep','ErrorNumber'],
  __sched_getaffinity:['','Unavailable'], // Kernel returns the actual mask byte count.
  __faccessat:['faccessat','AppendZero'], __fchmodat:['fchmodat','AppendZero'],
  __statfs64:['statfs64','Statfs'], __fstatfs64:['fstatfs64','Statfs'],
  __preadv64:['preadv64','VectorOffset'], __pwritev64:['pwritev64','VectorOffset'],
  __preadv64v2:['preadv64v2','VectorOffset'], __pwritev64v2:['pwritev64v2','VectorOffset'],
  __arm_fadvise64_64:['posix_fadvise64','Advice'],
  __sync_file_range2:['sync_file_range','SyncRange'],
  __rt_sigaction:['sigaction64','SignalSize'], __rt_sigpending:['sigpending64','SignalSize'],
  __rt_sigprocmask:['sigprocmask64','SignalSize'], __rt_sigsuspend:['sigsuspend64','SignalSize'],
  __rt_sigtimedwait:['sigtimedwait64','SignalSize'],
  __ppoll:['ppoll64','SignalSize'], __epoll_pwait:['epoll_pwait64','SignalSize'],
  __epoll_pwait2:['','Unavailable'], // Kernel time64 differs from ARM32 timespec.
  __signalfd4:['signalfd64','SignalFd'],
  __set_tls:['','Tls'], cacheflush:['','CacheFlush'],
};

function constants(include,output,target,entries,headers=[],clang='clang'){
  const file=path.join(output,target+'-numbers.c');
  writeChanged(file,'#include <asm/unistd.h>\n#include <asm-generic/errno.h>\n#include <limits.h>\n'+headers.map(header=>'#include <'+header+'>\n').join('')+entries.map(([name,value])=>`const unsigned ${name}=${value};`).join('\n')+'\n');
  const arch=target==='arm'?'arm-linux-androideabi':'aarch64-linux-android';
  const ir=execFileSync(clang,['--target='+arch+'36','-ffreestanding','-S','-emit-llvm','-I'+include,'-I'+path.join(include,arch),file,'-o','-'],{encoding:'utf8'});
  const values=new Map([...ir.matchAll(/^@(\w+) = .*constant i32 (-?\d+)/gm)].map(m=>[m[1],Number(m[2])>>>0]));
  for(const [name] of entries)if(!values.has(name))throw Error('Original syscall constant was not emitted: '+name);
  return values;
}

/** Kernel operation values come from the same original UAPI for both VMs;
 * the shared syscall adapter owns only their pointer/word representations. */
export function prepareFutexConstants({include,output,clang}){
  fs.mkdirSync(output,{recursive:true});
  const header=fs.readFileSync(path.join(include,'linux/futex.h'),'utf8');
  const names=[...header.matchAll(/^#define\s+(FUTEX_\w+)\s+[^\n]+/gm)].map(match=>match[1]);
  const entries=names.map(name=>['futex_'+name.slice(6),name]);
  const values=constants(include,output,'arm',entries,['linux/futex.h'],clang);
  writeChanged(path.join(output,'futex-abi.inc'),'// Original Linux futex UAPI values, evaluated by Clang.\n'+
    entries.map(([name])=>'[[maybe_unused]] static constexpr uint32_t '+name+'='+values.get(name)+'u;').join('\n')+'\n');
  return Object.fromEntries(values);
}

export function prepareSyscalls({bionicSource,include,python,importsDirectory,output}){
  fs.mkdirSync(output,{recursive:true});
  preparePrctlConstants({bionicSource,output});
  const source=path.join(output,'source');
  exportSources(bionicSource,pins.aospBionic,['libc/SYSCALLS.TXT','libc/tools/gensyscalls.py','libc/NOTICE'],source);
  const rows=JSON.parse(execFileSync(python,[path.join(import.meta.dirname,'syscalls-parse.py'),path.join(source,'libc/tools/gensyscalls.py'),path.join(source,'libc/SYSCALLS.TXT')],{encoding:'utf8'}));
  const ast=JSON.parse(fs.readFileSync(path.join(importsDirectory,'guest-ast.json')));
  const extra=[['Futex','__NR_futex'],['FutexTime64','__NR_futex_time64'],['NoSys','ENOSYS'],['Fault','EFAULT'],['Invalid','EINVAL'],['Range','ERANGE'],['MaxIov','IOV_MAX']];
  const numbers=constants(include,output,'arm',[...rows.map((row,i)=>['number'+i,row.macro]),...extra]);
  const host=constants(include,output,'arm64',[['Futex64','__NR_futex']]);
  prepareFutexConstants({include,output});
  const outputRows=rows.map((row,i)=>{
    const override=adaptation[row.func];
    let name=override?.[0]??(ast.functions[row.func]?row.func:row.func.replace(/^__/,''));
    let mode=override?.[1]??'Direct';
    const type=ast.types[ast.functions[name]?.type];
    // Bionic's private wrappers can add libc policy or change layouts. Only
    // equal fixed parameter widths enter the ordinary path. Distinct forms
    // above have explicit representation conversion or retain ENOSYS.
    if(!override && (!type || type.variadic || type.parameters.length!==row.registers.length || type.parameters.some((p,n)=>Math.ceil(ast.types[p].size/4)!==row.registers[n][1])))mode='Unavailable';
    if(mode!=='Unavailable' && !['OpenAt','Ptrace','Tls','CacheFlush'].includes(mode) && !['__mmap2','prctl'].includes(name) && (!type || type.variadic))mode='Unavailable';
    return {...row,number:numbers.get('number'+i),service:name,mode,voidResult:type&&ast.types[type.result]?.kind==='Void'};
  });
  const lines=['// Generated from complete pinned Bionic ARM syscall stubs and NDK numbers.'];
  for(const [name]of extra)lines.push(`static constexpr uint32_t syscall${name}=${numbers.get(name)};`);
  lines.push(`static constexpr uint32_t syscallFutex64=${host.get('Futex64')};`);
  lines.push('static constexpr Syscall syscalls[]={');
  for(const row of outputRows)lines.push(`{${row.number},${JSON.stringify(row.name)},${JSON.stringify(row.service)},SyscallMode::${row.mode},${row.voidResult?'true':'false'},${row.registers.length},{${row.registers.map(([offset,words])=>`{${offset},${words}}`).join(',')}}},`);
  lines.push('};');writeChanged(path.join(output,'syscalls.inc'),lines.join('\n')+'\n');
  writeChanged(path.join(output,'syscalls.json'),JSON.stringify(outputRows,null,2)+'\n');
  const licenses=path.join(output,'licenses');
  addLicense(path.dirname(licenses),'syscall-sources',path.join(source,'libc/NOTICE'));
  return {directory:output,licenses,entries:outputRows.length};
}
