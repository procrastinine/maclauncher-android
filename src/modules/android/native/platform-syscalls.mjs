import fs from 'node:fs';
import path from 'node:path';
import {readSyscallStubs,kernelSyscalls} from './process-abi/syscalls.mjs';

// Only differences between kernel and libc representations belong here. The
// original target UAPI and Bionic table supply membership; Clang supplies the
// complete declaration set. Ordinary adapters are never selected by name.
const adaptation={
  bpf:{name:'bpf',args:['uint32_t','void *','uint32_t'],returns:'int32_t'},
  __brk:{kernel:'The kernel returns an address; libc brk returns a status.'},
  __exit:{kernel:'Thread exit belongs to the physical thread executor.'},
  __clone:{kernel:'Thread creation belongs to the physical thread executor.'},
  __timer_create:{kernel:'Kernel timer IDs differ from libc timer_t.'},
  __timer_settime:{kernel:'Kernel timer IDs differ from libc timer_t.'},
  __timer_gettime:{kernel:'Kernel timer IDs differ from libc timer_t.'},
  __timer_getoverrun:{kernel:'Kernel timer IDs differ from libc timer_t.'},
  __timer_delete:{kernel:'Kernel timer IDs differ from libc timer_t.'},
  __openat:{name:'openat',args:['int32_t','void *','int32_t','uint32_t']},
  __ptrace:{name:'ptrace',args:['int32_t','int32_t','void *','void *']},
  __ioctl:{name:'ioctl',args:['int32_t','uint64_t','void *']},
  __fcntl:{name:'fcntl',args:['int32_t','int32_t','uint64_t']},
  prctl:{args:['int32_t','uint64_t','uint64_t','uint64_t','uint64_t']},
  mremap:{args:['void *','uint64_t','uint64_t','int32_t','void *']},
  __faccessat:{name:'faccessat',values:[1,2,3,0]},
  __fchmodat:{name:'fchmodat',values:[1,2,3,0]},
  __getpriority:{name:'getpriority',result:'priority'},
  __getcwd:{name:'getcwd',result:'length'},
  __getdents64:{name:'getdents64',args:['int32_t','void *','uint64_t'],returns:'int64_t'},
  __clock_nanosleep:{name:'clock_nanosleep',result:'errno'},
  __fadvise64:{name:'posix_fadvise64',result:'errno'},
  __sched_getaffinity:{name:'__sched_getaffinity',args:['int32_t','uint64_t','void *'],returns:'int32_t'},
  __preadv64:{name:'preadv64',values:[1,2,3,4]},
  __pwritev64:{name:'pwritev64',values:[1,2,3,4]},
  __preadv64v2:{name:'preadv64v2',values:[1,2,3,4,6]},
  __pwritev64v2:{name:'pwritev64v2',values:[1,2,3,4,6]},
  __waitid:{name:'waitid',values:[1,2,3,4],guard:'if (a5) break;'},
  __ppoll:{name:'ppoll',values:[1,2,3,4],signalSize:5,signalMask:4},
  __epoll_pwait:{name:'epoll_pwait',values:[1,2,3,4,5],signalSize:6,signalMask:5},
  __epoll_pwait2:{name:'epoll_pwait2',values:[1,2,3,4,5],signalSize:6,signalMask:5},
  __signalfd4:{name:'signalfd',values:[1,2,4],signalSize:3},
  __rt_sigaction:{name:'__rt_sigaction',args:['int32_t','void *','void *','uint64_t'],returns:'int32_t'},
  __rt_sigpending:{name:'sigpending64',values:[1],signalSize:2},
  __rt_sigprocmask:{name:'sigprocmask64',values:[1,2,3],signalSize:4},
  __rt_sigsuspend:{name:'sigsuspend64',values:[1],signalSize:2},
  __rt_sigtimedwait:{name:'sigtimedwait64',values:[1,2,3],signalSize:4},
  __rt_tgsigqueueinfo:{name:'rt_tgsigqueueinfo',args:['int32_t','int32_t','int32_t','void *'],returns:'int32_t'},
  __rt_sigqueueinfo:{name:'rt_sigqueueinfo',args:['int32_t','int32_t','void *'],returns:'int32_t'},
  __getcpu:{name:'getcpu',args:['void *','void *','void *'],returns:'int32_t'},
};

function scalar(ast,name){
  const type=ast.types[name];
  if(!type)return null;
  if(type.kind==='Pointer')return 'void *';
  if(type.kind==='Void')return 'void';
  if(type.kind==='Enum')return scalar(ast,type.underlying);
  if(![1,2,4,8].includes(type.size))return null;
  if(['Bool','Char_U','UChar','UShort','UInt','ULong','ULongLong'].includes(type.kind))return `uint${type.size*8}_t`;
  if(['Char_S','SChar','Short','Int','Long','LongLong'].includes(type.kind))return `int${type.size*8}_t`;
  return null;
}

export function emitPlatformSyscalls({rows,kernel,declarations:ast}){
  const stubs=new Map(rows.map(row=>[row.macro,row]));
  const entries=[],cases=[],seen=new Set();
  for(const raw of kernel){
    const row=stubs.get(raw.macro)||{func:raw.name,aliases:[]};
    // A raw-only name must use the same ABI rule as its Bionic private wrapper.
    const rule=adaptation[row.func]||adaptation['__'+row.func]||{};
    const names=[row.func,row.func.replace(/^__/,''),...(row.aliases||[])];
    const name=rule.name||names.find(name=>ast.functions[name])||names[0];
    const signature=ast.types[ast.functions[name]?.type];
    const args=rule.args||signature?.parameters.map(p=>scalar(ast,p));
    const returns=rule.returns||scalar(ast,signature?.result);
    const values=rule.values||args?.map((_,i)=>i+1);
    let reason=rule.kernel;
    if(!reason&&(!args||!returns||args.some(t=>!t)||(!rule.args&&signature?.variadic)))reason='The physical kernel owner handles the distinct declaration ABI.';
    if(!reason&&(args.length>6||args.length!==values.length||(!rule.values&&row.registers&&args.length!==row.registers.length)))reason='The physical kernel owner handles the distinct register layout.';
    const entry={...raw,stub:row.func,service:name,mode:reason?'kernel':'adapter',reason:reason||'Shared native import adapter; original target declaration.'};
    entries.push(entry);
    if(reason||seen.has(raw.number))continue;
    seen.add(raw.number);
    const invoke=`call(${args.map((type,i)=>`(${type})${type.includes('*')?'(uintptr_t)':''}${values[i]?'a'+values[i]:'0'}`).join(', ')})`;
    let body=returns==='void'?`${invoke}; return 0;`:`return (long)${invoke};`;
    if(rule.result==='priority')body=`errno = 0; int32_t value = ${invoke}; return value == -1 && errno ? -1 : 20 - value;`;
    if(rule.result==='length')body=`void *value = ${invoke}; return value ? (long)strlen(value) + 1 : -1;`;
    if(rule.result==='errno')body=`int32_t value = ${invoke}; if (value) { errno = kl_errno_from_linux(value); return -1; } return 0;`;
    // Linux set_user_sigmask ignores sigsetsize when no temporary mask is
    // supplied. Other signal operations still require their kernel ABI size.
    cases.push(`        case ${raw.number}: {\n            ${rule.guard||''}\n            ${rule.signalSize?`if (${rule.signalMask?`a${rule.signalMask} && `:''}a${rule.signalSize} != 8) { errno = EINVAL; return -1; }`:''}\n            ${returns} (*call)(${args.length?args.join(', '):'void'}) = kl_shim_lookup(${JSON.stringify(name)});\n            if (!call) break;\n            ${body}\n        }`);
  }
  return {entries,dispatch:`\n    switch (n) {\n${cases.join('\n')}\n    }\n`};
}

/** Raw calls and imported libc calls enter the same physical adapters. Missing
 * native adapters retain the original kernel dispatcher, including futex and
 * thread ownership, rather than introducing an earlier rejection. */
export function preparePlatformSyscalls(file,{bionicSource,include,python,importsDirectory,output}){
  const source=fs.readFileSync(file,'utf8');
  const signature='long klb_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6) {';
  if(source.split(signature).length!==2)throw Error('Klepton syscall entry changed');
  const {
    rows
  }=readSyscallStubs({bionicSource,python,output,architecture:'arm64'});
  const kernel=kernelSyscalls(include,output,'arm64');
  const declarations=JSON.parse(fs.readFileSync(path.join(importsDirectory,'host-ast.json')));
  const generated=emitPlatformSyscalls({rows,kernel,declarations});
  const preamble='#include <stdint.h>\n#include <string.h>\nextern void *kl_shim_lookup(const char *);\nextern int kl_errno_from_linux(int);\n';
  fs.writeFileSync(file,source.replace(signature,preamble+signature+generated.dispatch));
  fs.writeFileSync(path.join(output,'syscalls.json'),JSON.stringify(generated.entries,null,2)+'\n');
  return {
    entries:generated.entries
  };
}
