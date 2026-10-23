import fs from 'node:fs';
import path from 'node:path';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';

/** Resolve typed native argument blocks without changing their guest ABI. */
export function prepareHostcallMemory(aim){
 const edit=(file,changes)=>{let text=fs.readFileSync(file,'utf8');for(const[from,to]of changes){if(text.split(from).length!==2)throw Error('Original host-call argument boundary changed: '+file);text=text.replace(from,to);}writeChanged(file,text);};
 const host=path.join(aim,'crates/aim-hostcall/src'),abi=path.join(aim,'crates/aim-linux-abi/src');
 copyChanged(path.join(import.meta.dirname,'native/hostcall-arguments.rs'),path.join(host,'maclauncher_memory.rs'));
 copyChanged(path.join(import.meta.dirname,'native/hostcall-memory.rs'),path.join(abi,'sys/hostcall-memory.rs'));
 edit(path.join(host,'lib.rs'),[
  ['#![no_std]','#![no_std]\n\n#[cfg(target_os="macos")]\n#[path="maclauncher_memory.rs"]\npub mod maclauncher_memory;'],
  ['    // SAFETY: checked above; the caller vouches for the memory.','    #[cfg(target_os="macos")]\n    let args=unsafe{maclauncher_memory::argument(args,len,core::mem::align_of::<T>())}?;\n    // SAFETY: checked above; the caller vouches for the memory.'],
 ]);
 edit(path.join(abi,'sys/mod.rs'),[['mod jit;','mod jit;\n#[path="hostcall-memory.rs"]\npub(crate) mod hostcall_memory;']]);
 edit(path.join(abi,'hostcall.rs'),[
  ['        let call: ExtensionCall = std::mem::transmute(address);','        if let Err(error)=crate::sys::hostcall_memory::install_extension(handle){libc::dlclose(handle);return Err(error); }\n        let call: ExtensionCall = std::mem::transmute(address);'],
  ['pub fn call(module: u64, func: u64, args: u64, len: u64) -> i64 {',`pub fn call(module:u64,func:u64,args:u64,len:u64)->i64{
    if !crate::sys::mutable_memory::has_mappings(){return call_direct(module,func,args,len);}
    let _scope=crate::sys::hostcall_memory::Scope::enter();
    call_direct(module,func,args,len)
}
fn call_direct(module: u64, func: u64, args: u64, len: u64) -> i64 {`]]);
}
