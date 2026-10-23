//! Preserve the already-running HotSpot's native signal owner.
use super::*;
use std::{ffi::c_void,sync::OnceLock};
type JvmHandler=unsafe extern "C" fn(i32,*mut libc::siginfo_t,*mut c_void,i32)->i32;
type ChainedAction=unsafe extern "C" fn(i32)->*mut libc::sigaction;
struct Owner{jvm:Option<JvmHandler>,keep:[bool;32]}
static OWNER:OnceLock<Owner>=OnceLock::new();
pub(crate) fn enable(handler:*mut c_void)->Result<(),i64>{
 if handler.is_null(){return Err(-22);}let mut keep=[false;32];
 let chained=unsafe{libc::dlsym(libc::RTLD_DEFAULT,c"JVM_get_signal_action".as_ptr())};
 let jsig=if chained.is_null(){None}else{Some(unsafe{std::mem::transmute::<*mut c_void,ChainedAction>(chained)})};
 // The mere presence of a late-dlopen'ed library is insufficient. Its original
 // JVM initialization hooks must already have registered the VM's signal set.
 if let Some(action)=jsig{for signal in [libc::SIGSEGV,libc::SIGBUS,libc::SIGILL,libc::SIGFPE]{if unsafe{action(signal)}.is_null(){return Err(-16);}}}
 for signal in 1..32{let mut action:libc::sigaction=unsafe{std::mem::zeroed()};if unsafe{libc::sigaction(signal,std::ptr::null(),&mut action)}!=0{continue;}
  if signal==CARRIER&&action.sa_sigaction!=libc::SIG_DFL&&action.sa_sigaction!=libc::SIG_IGN{return Err(-16);}
  let vm_chained=jsig.is_some_and(|get|!unsafe{get(signal)}.is_null());
  keep[signal as usize]=(action.sa_sigaction!=libc::SIG_DFL||vm_chained)&&!matches!(signal,libc::SIGSEGV|libc::SIGBUS|libc::SIGILL|libc::SIGFPE|libc::SIGTRAP)&&signal!=CARRIER;
 }
 let jvm=if jsig.is_some(){None}else{Some(unsafe{std::mem::transmute::<*mut c_void,JvmHandler>(handler)})};
 OWNER.set(Owner{jvm,keep}).map_err(|_|-16)?;
 eprintln!("Bionic process signal chain: {}",if jsig.is_some(){"original initialized libjsig"}else{"explicit original JVM handler"});Ok(())
}
pub(crate) fn keep_host_handler(signal:i32)->bool{OWNER.get().is_some_and(|owner|owner.keep.get(signal as usize).copied().unwrap_or(false))}
pub(crate) unsafe fn forward_host(signal:i32,info:*mut libc::siginfo_t,uc:*mut c_void)->bool{
 let Some(owner)=OWNER.get()else{return false;};
 // With original libjsig the VM is already first and reaches this handler only
 // for an unrecognized fault. Calling the VM again would recurse through it.
 let Some(jvm)=owner.jvm else{return false;};
 if !matches!(signal,libc::SIGSEGV|libc::SIGBUS|libc::SIGILL|libc::SIGFPE|libc::SIGTRAP){return false;}
 let ctx=context::current_ctx();if !ctx.is_null()&&unsafe{(*ctx).in_host==0}{return false;}
 // Original HotSpot's documented handler receives the untouched kernel frame.
 // An unrecognized fault still reaches original AIM recovery/fatal handling.
 unsafe{jvm(signal,info,uc,0)!=0}
}
