//! Private additive foreign physical-thread owner. Bionic creates every guest
//! byte through original pthread_create. This module only chooses which real
//! Darwin pthread runs the resulting original clone child and borrows the
//! original syscall dispatcher for finite, returning entries.
use super::*;
use std::cell::Cell;
use std::sync::atomic::{AtomicU32,Ordering};
const CLAIMED:u32=5;const RESERVED:u32=0;const PREPARED:u32=1;const ACTIVE:u32=2;const DORMANT:u32=3;const RETIRED:u32=4;
pub(crate) const YIELD_NR:u64=0x4d4c1000;
struct State{boot:Option<Box<Boot>>,thread:Option<Arc<Thread>>,tp:u64,scs:u64}
struct Ticket{token:u64,pthread:usize,borrowed_tid:Option<i32>,phase:AtomicU32,state:Mutex<State>}
static TICKETS:LazyLock<Mutex<HashMap<u64,Arc<Ticket>>>>=LazyLock::new(||Mutex::new(HashMap::new()));
thread_local!{static ARMED:Cell<u64>=const{Cell::new(0)};static RUNNING:Cell<u64>=const{Cell::new(0)};}
fn errno(code:i32)->i64{-(code as i64)}
fn ticket(token:u64)->Result<Arc<Ticket>,i64>{TICKETS.lock().unwrap().get(&token).cloned().ok_or(errno(crate::errno::ESRCH))}
fn own(token:u64)->Result<Arc<Ticket>,i64>{let value=ticket(token)?;if value.pthread!=unsafe{libc::pthread_self()as usize}{return Err(errno(crate::errno::EPERM));}Ok(value)}
struct NativeErrno(i32);impl NativeErrno{fn save()->Self{Self(unsafe{*libc::__error()})}}impl Drop for NativeErrno{fn drop(&mut self){unsafe{*libc::__error()=self.0;}}}
#[repr(C)]pub(crate)struct Call{function:u64,argument:u64,result:u64}
#[repr(C)]pub(crate)struct Api{
 size:u32,version:u32,reserve:unsafe extern "C" fn(*mut u64)->i64,arm:extern "C" fn(u64)->i64,disarm:extern "C" fn(u64)->i64,
 invoke:unsafe extern "C" fn(u64,u64,u64,*mut u64)->i64,retire:extern "C" fn(u64)->i64,tp:extern "C" fn()->u64,context:extern "C" fn()->u64,reject_partial_entry:extern "C" fn(u64)->i64,
}
static API:Api=Api{size:std::mem::size_of::<Api>()as u32,version:1,reserve,arm,disarm,invoke,retire,tp,context:ctx,reject_partial_entry:reject_partial_entry};
pub(crate) unsafe fn install(handle:*mut libc::c_void){
 let address=unsafe{libc::dlsym(handle,c"maclauncher_bionic_admission_install".as_ptr())};
 if !address.is_null(){let install:unsafe extern "C" fn(*const Api)=unsafe{std::mem::transmute(address)};unsafe{install(&API);}}
}
unsafe extern "C" fn reserve(output:*mut u64)->i64{
 let _errno=NativeErrno::save();if output.is_null()||!context::current_ctx().is_null(){return errno(crate::errno::EINVAL);}
 let borrowed_tid=match crate::process_identity::thread(){Some(Ok(id))=>Some(id),Some(Err(error))=>return error,None=>None};
 let pthread=unsafe{libc::pthread_self()as usize};let mut table=TICKETS.lock().unwrap();
 if table.values().any(|value|value.pthread==pthread){return errno(crate::errno::EBUSY);}
 let mut nonce=0u64;loop{if unsafe{libc::getentropy((&mut nonce as *mut u64).cast(),8)}!=0{return errno(crate::errno::EIO);}if nonce!=0&&!table.contains_key(&nonce){break;}}
 let value=Arc::new(Ticket{token:nonce,pthread,borrowed_tid,phase:AtomicU32::new(RESERVED),state:Mutex::new(State{boot:None,thread:None,tp:0,scs:0})});table.insert(nonce,value);unsafe{output.write(nonce);}0
}
extern "C" fn arm(token:u64)->i64{
 let _errno=NativeErrno::save();let Ok(value)=ticket(token)else{return errno(crate::errno::ESRCH)};
 if context::current_ctx().is_null()||context::guest_tp()==0||!matches!(value.phase.load(Ordering::Acquire),RESERVED|DORMANT){return errno(crate::errno::EINVAL);}
 ARMED.with(|armed|{if armed.get()!=0{return errno(crate::errno::EBUSY);}armed.set(token);0})
}
extern "C" fn disarm(token:u64)->i64{let _errno=NativeErrno::save();ARMED.with(|armed|{if armed.get()==token{armed.set(0);0}else if armed.get()==0{0}else{errno(crate::errno::EPERM)}})}
/// Claimed before original clone ID allocation/publication. A borrowed
/// canonical lease remains owned by its real Java/Mach thread.
pub(super) struct CloneClaim{value:Arc<Ticket>,prior:u32}
impl Drop for CloneClaim{fn drop(&mut self){
 // Current upstream clone can fail after allocation (for example cgroup admission).
 // A successful capture has already advanced the phase and keeps its Boot.
 let _=self.value.phase.compare_exchange(CLAIMED,self.prior,Ordering::AcqRel,Ordering::Acquire);
}}
pub(super) fn claim_clone()->Result<Option<CloneClaim>,i64>{
 let token=ARMED.with(|armed|armed.replace(0));if token==0{return Ok(None);}
 let value=ticket(token)?;let prior=value.phase.load(Ordering::Acquire);
 if !matches!(prior,RESERVED|DORMANT)||value.phase.compare_exchange(prior,CLAIMED,Ordering::AcqRel,Ordering::Acquire).is_err(){return Err(errno(crate::errno::EBUSY));}
 Ok(Some(CloneClaim{value,prior}))
}
pub(super) fn borrowed_tid(claim:Option<&CloneClaim>,table:&HashMap<i32,Arc<Thread>>)->Result<Option<i32>,i64>{
 let Some(claim)=claim else{return Ok(None)};
 if let Some(id)=claim.value.borrowed_tid{if table.contains_key(&id){return Err(errno(crate::errno::EBUSY));}if claim.prior!=RESERVED{return Err(errno(crate::errno::EINVAL));}return Ok(Some(id));}
 if claim.prior!=RESERVED{return Err(errno(crate::errno::EBUSY));}Ok(None)
}
pub(super) fn failed_clone(claim:Option<&CloneClaim>){if let Some(claim)=claim{claim.value.phase.store(claim.prior,Ordering::Release);}}
/// Called at the original clone's final host-thread choice. No TLS layout,
/// child registers, ptid/ctid or native identity are synthesized or overwritten.
pub(super) unsafe fn capture(boot:*mut Boot,flags:u64,claim:Option<CloneClaim>)->Option<i64>{
 let claim=claim?;let value=claim.value.clone();assert_eq!(claim.prior,RESERVED);
 assert_eq!(flags&(CLONE_VM|CLONE_SIGHAND|CLONE_THREAD|CLONE_SETTLS|CLONE_PARENT_SETTID|CLONE_CHILD_CLEARTID),CLONE_VM|CLONE_SIGHAND|CLONE_THREAD|CLONE_SETTLS|CLONE_PARENT_SETTID|CLONE_CHILD_CLEARTID);
 assert_eq!(value.phase.load(Ordering::Acquire),CLAIMED);let boot=unsafe{Box::from_raw(boot)};assert_ne!(boot.tp,0);let tid=boot.thread.tid;
 if let Some(expected)=value.borrowed_tid{assert_eq!(expected,tid);}
 {let mut state=value.state.lock().unwrap();assert!(state.boot.is_none());state.tp=boot.tp;state.boot=Some(boot);}
 value.phase.store(PREPARED,Ordering::Release);Some(tid as i64)
}
pub(crate) fn yield_guest(ctx:&mut GuestContext,a:[u64;6])->i64{
 let token=RUNNING.with(Cell::get);let Ok(value)=own(token)else{return errno(crate::errno::EPERM)};
 if value.phase.load(Ordering::Acquire)!=ACTIVE||ctx.foreign_frame==0||ctx.callback_sp!=0||!matches!(a[0],1|2){return errno(crate::errno::EINVAL);}
 ctx.foreign_reason=a[0];0
}
pub(crate) fn is_current()->bool{RUNNING.with(|running|running.get()!=0)}
/// The original exit cleanup is shared with physical guest exits. Context and
/// native stack retirement are delayed only until the normal dispatcher returns.
pub(crate) fn exit_guest(a:[u64;6])->i64{
 let context=context::current_ctx();if context.is_null()||unsafe{(*context).callback_sp!=0}{return errno(crate::errno::EBUSY);}
 let token=RUNNING.with(Cell::get);let value=own(token).expect("admitted exit owner");
 let (ctx,stacks,main)=finish_thread_exit(a[0]as i32&0xff,false);assert!(!main);
 let thread=unsafe{&*(*ctx).thread};*thread.stacks.lock().unwrap()=stacks;
 unsafe{(*ctx).foreign_reason=3;}value.phase.store(RETIRED,Ordering::Release);0
}
extern "C" fn tp()->u64{if context::current_ctx().is_null(){0}else{context::guest_tp()}}
extern "C" fn ctx()->u64{context::current_ctx()as u64}
unsafe extern "C" fn invoke(token:u64,function:u64,argument:u64,result:*mut u64)->i64{
 let _errno=NativeErrno::save();if function==0||result.is_null(){return errno(crate::errno::EINVAL);}
 let value=match own(token){Ok(value)=>value,Err(error)=>return error};
 if RUNNING.with(Cell::get)==token{
  let context=context::current_ctx();if context.is_null()||unsafe{(*context).callback_sp==0||(*context).in_host==0}{return errno(crate::errno::EBUSY);}
  unsafe{result.write(context::call_guest(function,argument));}return 0;
 }
 let mut call=Call{function,argument,result:0};let outcome=run(&value,&mut call,false,false);let restore=context::take_foreign_restore_error();if restore!=0{return restore;}match outcome{Ok(())=>{unsafe{result.write(call.result);}0},Err(error)=>error}
}
extern "C" fn retire(token:u64)->i64{
 let _errno=NativeErrno::save();let value=match own(token){Ok(value)=>value,Err(error)=>return error};
 if RUNNING.with(Cell::get)!=0{return errno(crate::errno::EBUSY);}
 if value.phase.load(Ordering::Acquire)==RESERVED{TICKETS.lock().unwrap().remove(&token);return 0;}
 let mut call=Call{function:0,argument:0,result:0};let outcome=run(&value,&mut call,true,false);let restore=context::take_foreign_restore_error();if restore!=0{return restore;}match outcome{Ok(())=>0,Err(error)=>error}
}
fn run(value:&Arc<Ticket>,request:&mut Call,stopping:bool,reject_activation:bool)->Result<(),i64>{
 if !context::current_ctx().is_null()||RUNNING.with(Cell::get)!=0{return Err(errno(crate::errno::EBUSY));}
 let phase=value.phase.load(Ordering::Acquire);if !matches!(phase,PREPARED|DORMANT){return Err(errno(crate::errno::EINVAL));}
 let host=context::ForeignHostBinding::capture()?;
 let (thread,tp,scs,fresh)={let state=value.state.lock().unwrap();
  let thread=if phase==PREPARED{state.boot.as_ref().ok_or(errno(crate::errno::EINVAL))?.thread.clone()}else{state.thread.as_ref().ok_or(errno(crate::errno::EINVAL))?.clone()};
  (thread,state.tp,state.scs,phase==PREPARED)};
 let context=thread.ctx;
 if fresh{crate::process_identity::adopt(thread.tid).map_err(|_|errno(crate::errno::EIO))?;}
 {let mut stacks=thread.stacks.lock().unwrap();
  if stacks.is_none(){*stacks=Some(unsafe{context::foreign_stacks(context)});}
  let stacks=stacks.as_ref().unwrap();let scs=if fresh{stacks.scs()}else{scs};
  unsafe{context::activate_foreign_with_size(context,tp,scs,stacks,context::foreign_alternate_size(reject_activation))?;}
 }
 host.resume_native_mask()?;
 // Commit ownership only after all fallible physical-thread admission steps.
 // A rejection retains the genuine Boot and original child for retry/ThreadEnd.
 if fresh{let mut state=value.state.lock().unwrap();state.thread=Some(thread.clone());state.boot.take();thread.pthread.store(unsafe{libc::pthread_self()as usize},SeqCst);super::super::procrec::running(thread.tid,host_id(thread.pthread.load(SeqCst)));}
 RUNNING.with(|running|running.set(value.token));value.phase.store(ACTIVE,Ordering::Release);
 let mut reason=unsafe{if !fresh{(*context).x[0]=request as *mut Call as u64;}context::klj_bionic_enter_context(context)};
 if fresh&&reason==1{unsafe{(*context).x[0]=request as *mut Call as u64;}reason=unsafe{context::klj_bionic_enter_context(context)};}
 {let mut state=value.state.lock().unwrap();state.tp=context::guest_tp();state.scs=context::guest_scs();}
 if reason==3{
  let stacks=thread.stacks.lock().unwrap().take();unsafe{context::unbind(stacks);drop(Arc::from_raw((*context).thread));drop(Box::from_raw(context));}
  TICKETS.lock().unwrap().remove(&value.token);
 }else{value.phase.store(DORMANT,Ordering::Release);}
 RUNNING.with(|running|running.set(0));host.restore()?;
 if reason==3&&stopping||reason==2&&!stopping{Ok(())}else{Err(errno(crate::errno::EIO))}
}

extern "C" fn reject_partial_entry(token:u64)->i64{let _errno=NativeErrno::save();let value=match own(token){Ok(value)=>value,Err(error)=>return error};if !matches!(value.phase.load(Ordering::Acquire),PREPARED|DORMANT)||!context::current_ctx().is_null(){return errno(crate::errno::EBUSY);}let mut call=Call{function:0,argument:0,result:0};let outcome=run(&value,&mut call,false,true);let restoration=context::take_foreign_restore_error();if restoration!=0{restoration}else{outcome.err().unwrap_or(errno(crate::errno::EIO))}}

#[cfg(test)]
mod admission_tests{
 use super::*;
 #[test]
 fn failed_clone_claim_leaves_the_real_ticket_retryable(){
  let mut token=0;assert_eq!(unsafe{reserve(&mut token)},0);
  let value=ticket(token).unwrap();
  for _ in 0..2{
   ARMED.with(|armed|armed.set(token));
   let claim=claim_clone().unwrap().unwrap();
   assert_eq!(value.phase.load(Ordering::Acquire),CLAIMED);
   // Same unwind as an upstream clone error before the capture boundary.
   drop(claim);assert_eq!(value.phase.load(Ordering::Acquire),RESERVED);
  }
  assert_eq!(retire(token),0);assert!(ticket(token).is_err());
 }
}
