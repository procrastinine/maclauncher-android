import fs from 'node:fs';
import path from 'node:path';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';

/** Darwin Mach copies reject MAP_JIT destinations; retain Linux user-copy errors. */
export function prepareJitUserCopy(aim){
 const root=path.join(aim,'crates/aim-linux-abi/src');
 const edit=(name,before,after)=>{const file=path.join(root,name),source=fs.readFileSync(file,'utf8');if(source.split(before).length!==2)throw Error('Original JIT user-copy boundary changed: '+name);writeChanged(file,source.replace(before,after));};
 copyChanged(path.join(import.meta.dirname,'native/jit-user-copy.rs'),path.join(root,'sys/jit-user-copy.rs'));
 edit('sys/mod.rs','pub(crate) mod user_memory;','pub(crate) mod user_memory;\n#[path="jit-user-copy.rs"]\npub(crate) mod jit_user_copy;');
 edit('sys/signal.rs','    let m = unsafe { sigframe::mcontext(uc) };','    let m = unsafe { sigframe::mcontext(uc) };\n    if matches!(hsig,libc::SIGSEGV|libc::SIGBUS)&&si.si_code>0&&si.si_code<0x10000&&super::jit_user_copy::recover(m){return None;}');
 edit('sys/user_memory.rs',`        let count = (bytes.len() - done).min(u32::MAX as usize);
        // SAFETY: source is our live slice; Mach validates the user destination.`,
 `        let at=address+done as u64;
        let (_,stop,protection,_)=crate::patch::vm::region(at).filter(|(start,stop,_,_)|*start<=at&&*stop>at).ok_or(errno::EFAULT)?;
        let count = (bytes.len() - done).min(u32::MAX as usize).min((stop-at)as usize);
        // SAFETY: source is our live slice; Mach validates the user destination.`);
 edit('sys/user_memory.rs',`        if result != 0 {
            return Err(errno::EFAULT);
        }
        done += count;`,
 `        if result != 0 {
            const RWX:i32=libc::PROT_READ|libc::PROT_WRITE|libc::PROT_EXEC;
            if !super::arena::contains(at,count as u64)||protection&RWX!=RWX{return Err(errno::EFAULT);}
            super::jit_user_copy::write(at,&bytes[done..done+count])?;
        }
        done += count;`);
}
