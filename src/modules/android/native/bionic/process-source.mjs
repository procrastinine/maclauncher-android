import fs from 'node:fs';import path from 'node:path';
import {copyChanged,writeChanged} from '../upstream/source-files.mjs';

/** Add finite initialization after prepareBionicThreads without selecting it. */
export function prepareBionicProcess(aim){
const work=import.meta.dirname,root=path.join(aim,'crates/aim-linux-abi'),src=path.join(root,'src');
function edit(file,pairs){const name=path.join(src,file);let text=fs.readFileSync(name,'utf8');for(const[from,to]of pairs){if(text.split(from).length!==2)throw Error('Original process boundary changed: '+file);text=text.replace(from,to);}writeChanged(name,text);}
const cargo=path.join(root,'Cargo.toml'),manifest=fs.readFileSync(cargo,'utf8'),lib='[lib]\npath = "src/lib.rs"';if(manifest.split(lib).length!==2)throw Error('Original ABI library declaration changed');writeChanged(cargo,manifest.replace(lib,lib+'\ncrate-type=["rlib","cdylib"]'));
edit('sys/mod.rs',[['mod signal;','pub(crate) mod signal;']]);
edit('lib.rs',[
 ['pub mod hostcall;','pub mod hostcall;\n#[path="process-entry.rs"] mod bionic_process;'],
 ['            context::enter_prepared_guest(guest)','            if let Some(result)=bionic_process::enter(&guest){return result;}\n            context::enter_prepared_guest(guest)']
]);
writeChanged(path.join(src,'context.rs'),fs.readFileSync(path.join(src,'context.rs'),'utf8')+'\npub(crate) fn foreign_prepared_context(guest:&PreparedGuest)->*mut GuestContext{guest.0}\n');
edit('sys/thread-admission.rs',[
 ['borrowed_tid:Option<i32>,phase:','borrowed_tid:Option<i32>,initial:bool,phase:'],
 ['Ticket{token:nonce,pthread,borrowed_tid,phase:','Ticket{token:nonce,pthread,borrowed_tid,initial:false,phase:'],
 [' if RUNNING.with(Cell::get)!=0{return errno(crate::errno::EBUSY);}',' if value.initial{return errno(crate::errno::EPERM);}\n if RUNNING.with(Cell::get)!=0{return errno(crate::errno::EBUSY);}']
]);
writeChanged(path.join(src,'sys/thread-admission.rs'),fs.readFileSync(path.join(src,'sys/thread-admission.rs'),'utf8')+`
/// Adopt the already-registered original initial thread; its TID is the real
/// native process leader, not another Java thread or a replacement identity.
pub(crate) unsafe fn initial_enter(ctx:*mut GuestContext)->Result<u64,i64>{
 let thread=current().ok_or(errno(crate::errno::EINVAL))?;
 if thread.ctx!=ctx||thread.tid!=pid()||RUNNING.with(Cell::get)!=0{return Err(errno(crate::errno::EPERM));}
 let thread=find(thread.tid).ok_or(errno(crate::errno::ESRCH))?;
 let pthread=unsafe{libc::pthread_self()as usize};let mut table=TICKETS.lock().unwrap();if table.values().any(|v|v.pthread==pthread){return Err(errno(crate::errno::EBUSY));}
 let mut nonce=0u64;loop{if unsafe{libc::getentropy((&mut nonce as *mut u64).cast(),8)}!=0{return Err(errno(crate::errno::EIO));}if nonce!=0&&!table.contains_key(&nonce){break;}}
 let value=Arc::new(Ticket{token:nonce,pthread,borrowed_tid:Some(thread.tid),initial:true,phase:AtomicU32::new(ACTIVE),state:Mutex::new(State{boot:None,thread:Some(thread),tp:0,scs:0})});table.insert(nonce,value.clone());drop(table);
 RUNNING.with(|running|running.set(nonce));let reason=unsafe{context::klj_bionic_enter_context(ctx)};
 {let mut state=value.state.lock().unwrap();state.tp=context::guest_tp();state.scs=context::guest_scs();}value.phase.store(DORMANT,Ordering::Release);RUNNING.with(|running|running.set(0));
 if reason==1{Ok(nonce)}else{Err(errno(crate::errno::EIO))}
}
`);
edit('sys/signal.rs',[
 ['// ---- host signal handler --------------------------------------------------------','#[path="embedded-signals.rs"]\npub(crate) mod embedded;\n\n// ---- host signal handler --------------------------------------------------------'],
 ['    // SAFETY: SA_SIGINFO handler arguments from the kernel.','    if unsafe{embedded::forward_host(sig,info,uc)}{return;}\n    // SAFETY: SA_SIGINFO handler arguments from the kernel.'],
 ['                && sig != SIGSTOP\n            {','                && sig != SIGSTOP\n                && !embedded::keep_host_handler(h)\n            {'],
 ['fn mirror_on_host(sig: i32, act: &KSigaction) {\n    let h = to_host(sig);','fn mirror_on_host(sig: i32, act: &KSigaction) {\n    let h = to_host(sig);\n    if embedded::keep_host_handler(h){return;}']
]);
for(const[from,to]of [['process-entry.rs','process-entry.rs'],['embedded-signals.rs','sys/embedded-signals.rs'],['native-poll-lease.rs','sys/native-poll-lease.rs'],['native-read-lease.rs','sys/native-read-lease.rs']])copyChanged(path.join(work,from),path.join(src,to));
edit('sys/fdtab.rs',[['pub struct Pinned{','#[path=\"native-poll-lease.rs\"] pub(crate) mod native_poll_lease;\n#[path=\"native-read-lease.rs\"] pub(crate) mod native_read_lease;\npub struct Pinned{']]);
}

