import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {writeChanged} from '../upstream/source-files.mjs';

// The signal number plus si_code selects the original Linux siginfo union.
// Clang supplies all values independently for both Android ABI declarations.
export function generateSignalAbi({include,output,clang='clang'}){
 const names=['SI_USER','SI_KERNEL','SI_TIMER','SI_SIGIO','SIGILL','SIGFPE','SIGSEGV','SIGBUS','SIGTRAP','SIGCHLD','SIGPOLL','SIGSYS','NSIGILL','NSIGFPE','NSIGSEGV','NSIGBUS','NSIGTRAP','NSIGCHLD','NSIGPOLL','NSIGSYS','BUS_MCEERR_AR','BUS_MCEERR_AO','SEGV_BNDERR','SEGV_PKUERR','TRAP_PERF'];
 const source='#include <signal.h>\n'+names.map(name=>`int klj_${name}(void){return ${name};}`).join('\n');
 const rows=[];
 for(const [side,target,arch]of [['guest','armv7a-linux-androideabi36','arm-linux-androideabi'],['host','aarch64-linux-android36','aarch64-linux-android']]){
  const ir=execFileSync(clang,['--target='+target,'-isystem',include,'-isystem',path.join(include,arch),'-x','c','-O2','-S','-emit-llvm','-o','-','-'],{input:source,encoding:'utf8'});
  const values=Object.fromEntries([...ir.matchAll(/define[^\n]*@klj_(\w+)\([^]*?ret i32 (-?\d+)/g)].map(match=>[match[1],Number(match[2])]));
  for(const name of names)if(!Number.isInteger(values[name]))throw Error('Original signal constant did not compile: '+name);
  rows.push(`static constexpr SignalAbi ${side}SignalAbi={${names.map(name=>values[name]).join(',')}};`);
 }
 fs.mkdirSync(output,{recursive:true});
 writeChanged(path.join(output,'signal-abi.inc'),'// Original Android signal.h evaluated by Clang for both ABIs.\nstruct SignalAbi{'+names.map(name=>'int v_'+name+';').join('')+'};\n'+rows.join('\n')+'\n');
}
