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
 // Upstream's exception-table transfer owns ordinary copy faults. Retain only
 // the prepared copy boundary used under the existing I/O mapping locks and
 // the co-resident executor's physical JIT-view preservation.
 const file=path.join(root,'sys/user_memory.rs');
 writeChanged(file,fs.readFileSync(file,'utf8')+`

pub(crate) fn write_prepared(address:u64,bytes:&[u8])->Result<(),Errno>{
    write_ready(address,bytes,false)
}
pub(crate) fn write_locked(address:u64,bytes:&[u8])->Result<(),Errno>{
    write_ready(address,bytes,true)
}
fn write_ready(address:u64,bytes:&[u8],locked:bool)->Result<(),Errno>{
    range(address,bytes.len())?;
    let mut done=0;
    while done<bytes.len(){
        let at=address+done as u64;
        let (start,stop,protection,_)=crate::patch::vm::region(at).ok_or(errno::EFAULT)?;
        if start>at||stop<=at||protection&libc::PROT_WRITE==0{return Err(errno::EFAULT);}
        let count=(bytes.len()-done).min((stop-at)as usize);
        // Prepared copyout holds the original mapping owner.
        if protection&7==7&&super::arena::contains(at,count as u64){
            if locked{super::jit_user_copy::write_locked(at,&bytes[done..done+count])?;}
            else{super::jit_user_copy::write(at,&bytes[done..done+count])?;}
        }else{transfer(bytes[done..].as_ptr()as u64,at,count)?;}
        done+=count;
    }
    Ok(())
}
`);
}
