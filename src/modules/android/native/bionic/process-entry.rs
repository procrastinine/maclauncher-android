//! Finite original process initialization, called on the actual Java first thread.
use crate::{RunOptions,context,sys};
use std::{cell::Cell,ffi::{CStr,c_char,c_void},path::{Path,PathBuf},sync::atomic::{AtomicU32,Ordering}};
thread_local!{static PREPARING:Cell<bool>=const{Cell::new(false)};static INITIAL_TOKEN:Cell<u64>=const{Cell::new(0)};}
static PHASE:AtomicU32=AtomicU32::new(0);
#[repr(C)]pub struct Options{size:u32,version:u32,root:*const c_char,path_map:*const c_char,program:*const c_char,runtime:*const c_char,host_module:*const c_char,identity:*const c_char,envp:*const *const c_char,envc:usize}
unsafe fn text<'a>(value:*const c_char)->Result<&'a str,i64>{if value.is_null(){return Err(-22);}unsafe{CStr::from_ptr(value)}.to_str().map_err(|_|-22)}
#[unsafe(no_mangle)]pub unsafe extern "C" fn klj_bionic_process_prepare(raw:*const Options,output:*mut u64,host_signal:*mut c_void)->i64{
 if raw.is_null()||output.is_null()||host_signal.is_null(){return -22;}
 let raw=unsafe{&*raw};if raw.size!=std::mem::size_of::<Options>()as u32||raw.version!=1{return -22;}
 if raw.envc>4096||(raw.envc!=0&&raw.envp.is_null()){return -22;}
 let envp=if raw.envc==0{Vec::new()}else{unsafe{std::slice::from_raw_parts(raw.envp,raw.envc)}.iter().map(|value|unsafe{CStr::from_ptr(*value)}.to_bytes().to_vec()).collect()};
 let parsed=(||->Result<_,i64>{Ok((unsafe{text(raw.root)}?,unsafe{text(raw.path_map)}?,unsafe{text(raw.program)}?,unsafe{text(raw.runtime)}?,unsafe{text(raw.host_module)}?,sys::cred::Identity::parse(unsafe{text(raw.identity)}?).map_err(|_|-22)?))})();
 let(root,map,program,runtime,module,identity)=match parsed{Ok(value)=>value,Err(error)=>return error};
 if PHASE.compare_exchange(0,1,Ordering::AcqRel,Ordering::Acquire).is_err(){return -16;}
 // The existing native owner remains authoritative. Never adopt its process
 // leader ID onto an unrelated already-enrolled Java worker.
 if let Err(error)=crate::hostcall::load_extension(Path::new(module)){eprintln!("Bionic process host module: {error}");PHASE.store(3,Ordering::Release);return -5;}
 let pid=match crate::process_identity::process(){Some(Ok(id))=>id,_=>return -22};
 if crate::process_identity::thread()!=Some(Ok(pid)){PHASE.store(3,Ordering::Release);return -1;}
 if let Err(error)=sys::signal::embedded::enable(host_signal){PHASE.store(3,Ordering::Release);return error;}
 // The existing exact copy-fault recovery also observes the initial physical JIT view.
 sys::signal::install_host_handlers();
 let host=match context::ForeignHostBinding::capture(){Ok(value)=>value,Err(error)=>{PHASE.store(3,Ordering::Release);return error;}};
 crate::vfs::use_runtime(PathBuf::from(runtime));PREPARING.with(|value|value.set(true));
 let outcome=crate::run(RunOptions{root:Path::new(root),path_map:Some(Path::new(map)),program,argv:vec![program.as_bytes().to_vec()],envp,execfn:None,trace:false,cache:None,binder:None,identity,by_pid:None,mount_namespace_from_init:false,posix_control:None,state:sys::ExecState::default(),runtime_args:vec![],guest_fds:vec![0,1,2],socket_receipts:vec![]});
 PREPARING.with(|value|value.set(false));let token=INITIAL_TOKEN.with(Cell::get);let restored=host.restore();
 if !outcome.is_empty()||token==0{eprintln!("Bionic process initialization: {outcome}");PHASE.store(3,Ordering::Release);return -5;}
 if let Err(error)=restored{PHASE.store(3,Ordering::Release);return error;}
 unsafe{output.write(token);}PHASE.store(2,Ordering::Release);0
}
pub(crate) fn enter(guest:&context::PreparedGuest)->Option<String>{
 if !PREPARING.with(Cell::get){return None;}
 let ctx=context::foreign_prepared_context(guest);
 match unsafe{sys::thread::admission::initial_enter(ctx)}{Ok(token)=>{INITIAL_TOKEN.with(|value|value.set(token));Some(String::new())},Err(error)=>Some(format!("original initial thread entry: errno {}",-error))}
}
