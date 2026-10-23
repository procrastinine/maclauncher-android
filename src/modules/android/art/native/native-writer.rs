//! Foreign Darwin writers use the original signal frame and precise CPU.
//! Mapping admission remains with the logical memory owner; no pointer fields
//! are rewritten and public original-code pages remain read-only/non-executable.
use std::ffi::c_void;
#[cfg(maclauncher_precise)]
mod engine {
 use super::*;
 use super::super::{mutable_memory as memory,verity_pager};
 use std::{os::fd::{AsRawFd,FromRawFd,OwnedFd},sync::{OnceLock,atomic::{AtomicU64,Ordering}}};
 const W:i32=libc::PROT_WRITE;const X:i32=libc::PROT_EXEC;
 use super::super::precise_ffi::{State,Mapping,Config as CpuConfig,kl_a64_precise_create,kl_a64_precise_destroy,kl_a64_precise_run_owned,kl_a64_precise_instruction,kl_a64_precise_instruction_address};
 #[repr(C)]struct Config{owner:*mut c_void,execute:unsafe extern "C" fn(*mut c_void,*mut State,u64)->i32,free:[i32;2],requests:[i32;2],completion:[[i32;2];4]}
 unsafe extern "C"{
  fn kl_native_writer_create(config:Config)->*mut c_void;
  fn kl_native_writer_destroy(owner:*mut c_void);
  fn kl_native_writer_region(address:u64,start:*mut u64,end:*mut u64,protection:*mut i32)->i32;
  fn kl_native_writer_fault(owner:*mut c_void,signal:i32,info:*const libc::siginfo_t,uc:*mut c_void)->i32;
 }
 const _:()=assert!(std::mem::size_of::<State>()==808);
 struct Descriptor(Option<OwnedFd>);
 impl AsRawFd for Descriptor{fn as_raw_fd(&self)->i32{self.0.as_ref().unwrap().as_raw_fd()}}
 impl Drop for Descriptor{fn drop(&mut self){
  use aim_storage::private_fd::Registrar;
  if let Some(fd)=self.0.take(){let _guard=super::super::fdtab::lifecycle();let raw=fd.as_raw_fd();drop(fd);super::super::fdtab::StorageRegistrar.closed(raw);}
 }}
 fn pipes()->std::io::Result<Vec<Descriptor>>{
  use aim_storage::private_fd::Registrar;
  let guard=super::super::fdtab::lifecycle();let mut raw=Vec::<OwnedFd>::new();
  for _ in 0..6{let mut pair=[-1;2];if unsafe{libc::pipe(pair.as_mut_ptr())}<0{return Err(std::io::Error::last_os_error());}for fd in pair{raw.push(unsafe{OwnedFd::from_raw_fd(fd)});}}
  // hide() can retain the original descriptor under a low NOFILE ceiling.
  // Its exec lifetime must therefore be closed before any registrar fallback.
  for fd in &raw{if unsafe{libc::fcntl(fd.as_raw_fd(),libc::F_SETFD,libc::FD_CLOEXEC)}<0{return Err(std::io::Error::last_os_error());}}
  let mut adopted=Vec::<OwnedFd>::new();
  for fd in raw{match super::super::fdtab::StorageRegistrar.adopt(fd){Ok(fd)=>adopted.push(fd),Err(error)=>{
   for fd in adopted{let raw=fd.as_raw_fd();drop(fd);super::super::fdtab::StorageRegistrar.closed(raw);}return Err(error);
  }}}
  drop(guard);Ok(adopted.into_iter().map(|fd|Descriptor(Some(fd))).collect())
 }
 #[derive(Default)]struct Memory{leases:Vec<memory::Lease>,wrote:bool}
 #[derive(Default)]struct Worker{cpu:*mut c_void,memory:Box<Memory>}
 impl Drop for Worker{fn drop(&mut self){if !self.cpu.is_null(){unsafe{kl_a64_precise_destroy(self.cpu);}}}}
 struct Owner{native:*mut c_void,_worker:Box<Worker>,_descriptors:Vec<Descriptor>}
 unsafe impl Send for Owner{}unsafe impl Sync for Owner{}
 impl Drop for Owner{fn drop(&mut self){unsafe{kl_native_writer_destroy(self.native);}}}
 static OWNER:OnceLock<Result<Owner,String>>=OnceLock::new();static STEP:AtomicU64=AtomicU64::new(1);
 unsafe extern "C" fn map(opaque:*mut c_void,address:u64,length:usize,required:u32,result:*mut Mapping)->i32{
  let worker=unsafe{&mut*opaque.cast::<Memory>()};let required=required as i32;let _=length;
  if let Some(lease)=memory::mapping(address,1,required){
   let page=address&!4095;let end=(page+4096).min(lease.start+lease.length);
   unsafe{*result=Mapping{address:page,length:end-page,host:(lease.address+page-lease.start)as*mut _,permissions:lease.protection as u32,flags:0};}
   worker.leases.push(lease);return 0;
  }
  if memory::covers(address,1){return -crate::errno::EFAULT;}
  let(mut start,mut stop,mut protection)=(0,0,0);
  if unsafe{kl_native_writer_region(address,&mut start,&mut stop,&mut protection)}!=0{return -crate::errno::EFAULT;}
  let page=address&!4095;let end=(page+4096).min(stop);
  if page<start||stop<=address||protection&required!=required{return -crate::errno::EFAULT;}
  let jit=protection&7==7&&super::super::arena::contains(page,end-page);
  unsafe{*result=Mapping{address:page,length:end-page,host:page as*mut _,permissions:protection as u32,flags:if jit{1}else{2}};}0
 }
 unsafe extern "C" fn write(opaque:*mut c_void,address:u64,length:usize){unsafe{&mut*opaque.cast::<Memory>()}.wrote|=memory::overlaps(address,length as u64);}
 unsafe extern "C" fn counter(_: *mut c_void)->u64{aim_hostcall::clock::monotonic_ns()as u64}
 unsafe extern "C" fn execute(opaque:*mut c_void,state:*mut State,address:u64)->i32{
  std::panic::catch_unwind(std::panic::AssertUnwindSafe(||{
   let worker=unsafe{&mut*opaque.cast::<Worker>()};let state=unsafe{&mut*state};
   let _epoch=verity_pager::mutation();let _transaction=memory::transaction();
   let Some(admission)=memory::mapping(address,1,W)else{return -crate::errno::EFAULT;};
   if worker.cpu.is_null(){
    let config=CpuConfig{owner:(&mut*worker.memory)as*mut _ as*mut _,map,counter,ctr:crate::a64::host_ctr_el0(),frequency:1_000_000_000,dczid:4,write};
    worker.cpu=unsafe{kl_a64_precise_create(config)};if worker.cpu.is_null(){return -crate::errno::ENOMEM;}
   }
   // An original STXR may first lose its interrupted native reservation. Keep
   // its actual conditional retry under the CPU owner and retained mapping
   // lease. Every callback still admits the exact native or logical permissions.
   for _ in 0..64{
    worker.memory.leases.clear();worker.memory.wrote=false;
    let result=unsafe{kl_a64_precise_run_owned(worker.cpu,state,STEP.fetch_add(1,Ordering::Relaxed),0,u64::MAX,64)};
    worker.memory.leases.clear();
    // Native syscalls run on the original caller, with its TLS, kernel thread
    // identity and signal frame. Unicorn has advanced past the SVC; the CPU
    // records its exact original instruction address separately.
    if result.kind==1&&result.detail==0x80&&unsafe{kl_a64_precise_instruction(worker.cpu)}==0xd4001001{
     state.pc=unsafe{kl_a64_precise_instruction_address(worker.cpu)};drop(admission);return 0;
    }
    if result.kind!=0{#[cfg(test)]eprintln!("native writer precise stop kind={} detail={} errno={} address={:#x} pc={:#x} instruction={:#x}",result.kind,result.detail,result.error,result.address,state.pc,unsafe{kl_a64_precise_instruction(worker.cpu)});return if result.error!=0{result.error}else{-crate::errno::EFAULT};}
    if worker.memory.wrote{drop(admission);return 0;}
   }
   // A bounded architecture stop retains actual progressed registers. No
   // reservation survives a public precise run or the original sigreturn.
   drop(admission);0
  })).unwrap_or(-crate::errno::EFAULT)
 }
 #[cfg(test)]pub(super) fn descriptors_cloexec()->bool{matches!(OWNER.get(),Some(Ok(owner))if owner._descriptors.iter().all(|fd|unsafe{libc::fcntl(fd.as_raw_fd(),libc::F_GETFD)}&libc::FD_CLOEXEC!=0))}
 pub(super) fn initialize()->Result<(),String>{
  OWNER.get_or_init(||{
   let descriptors=pipes().map_err(|error|error.to_string())?;let mut worker=Box::<Worker>::default();
   let config=Config{owner:(&mut*worker)as*mut _ as*mut _,execute,free:[descriptors[0].as_raw_fd(),descriptors[1].as_raw_fd()],requests:[descriptors[2].as_raw_fd(),descriptors[3].as_raw_fd()],completion:std::array::from_fn(|n|[descriptors[4+n*2].as_raw_fd(),descriptors[5+n*2].as_raw_fd()])};
   let native=unsafe{kl_native_writer_create(config)};if native.is_null(){return Err(std::io::Error::last_os_error().to_string());}
   Ok(Owner{native,_worker:worker,_descriptors:descriptors})
  }).as_ref().map(|_|()).map_err(Clone::clone)
 }
 pub(super) unsafe fn fault(ctx:*mut crate::context::GuestContext,signal:i32,info:&libc::siginfo_t,uc:*mut c_void)->bool{
  if !memory::has_mappings(){return false;}
  if (signal!=libc::SIGBUS&&signal!=libc::SIGSEGV)||info.si_code<=0||info.si_code>=0x10000{return false;}
  if !ctx.is_null()&&unsafe{(*ctx).in_host}==0{return false;}
  let m=unsafe{super::super::sigframe::mcontext(uc)};let ec=m.esr>>26;
  if !matches!(ec,0x24|0x25)||m.esr&(1<<6)==0||!memory::intent(m.far,W){return false;}
  let Some(Ok(owner))=OWNER.get()else{return false;};
  unsafe{kl_native_writer_fault(owner.native,signal,info,uc)!=0}
 }
}
pub(crate) fn initialize()->Result<(),String>{#[cfg(maclauncher_precise)]{engine::initialize()}#[cfg(not(maclauncher_precise))]{Ok(())}}
pub(crate) unsafe fn fault(ctx:*mut crate::context::GuestContext,signal:i32,info:&libc::siginfo_t,uc:*mut c_void)->bool{
 #[cfg(maclauncher_precise)]{unsafe{engine::fault(ctx,signal,info,uc)}}#[cfg(not(maclauncher_precise))]{let _=(ctx,signal,info,uc);false}
}

#[cfg(all(test,maclauncher_precise))]
mod tests{
 use super::*;use super::super::mutable_memory as memory;
 use std::{os::unix::process::ExitStatusExt,process::Command};
 const PAGE:usize=16384;const RW:i32=libc::PROT_READ|libc::PROT_WRITE;
 #[repr(C,align(16))]struct Registers{before:[u64;32],after:[u64;32],neon_before:[[u64;2];32],neon_after:[[u64;2];32],flags_before:u64,flags_after:u64}
 unsafe extern "C"{
  fn kl_native_writer_test_store(out:*mut u64,value:u64);
  fn kl_native_writer_test_copy(out:*mut c_void,input:*const c_void,length:usize);
  fn kl_native_writer_test_tls(out:*mut u64,value:u64)->u64;
  fn kl_native_writer_test_add(out:*mut u64,count:u32);
  fn kl_native_writer_test_lrsc(out:*mut u64);
  fn native_store_probe(out:*mut u64,value:u64,result:*mut Registers);
  fn native_svc_retry(out:*mut u64,thread:*mut u64);
  fn native_cas_retry16(out:*mut u16,expected:u16,value:u16)->u16;
 }
 fn mapping(protection:i32)->u64{
  let raw=unsafe{libc::mmap(std::ptr::null_mut(),PAGE,RW,libc::MAP_ANON|libc::MAP_PRIVATE,-1,0)};
  assert_ne!(raw,libc::MAP_FAILED);let base=raw as u64;
  {let _epoch=super::super::verity_pager::mutation();let _guard=memory::transaction();memory::prepare(base,&vec![0;PAGE],protection).unwrap().install(|publish|publish()).unwrap();}base
 }
 fn execute(base:u64,background:bool){
  if background{assert!(crate::context::current_ctx().is_null());}else{let ctx=crate::context::init_thread();assert_eq!(unsafe{(*ctx).in_host},1);}
  let mut before=unsafe{std::mem::zeroed()};unsafe{libc::sigemptyset(&mut before);libc::sigaddset(&mut before,libc::SIGUSR1);assert_eq!(libc::pthread_sigmask(libc::SIG_SETMASK,&before,std::ptr::null_mut()),0);*libc::__error()=libc::EDOM;}
  let value=if background{0x44556677}else{0x11223344};
  assert_eq!(unsafe{kl_native_writer_test_tls((base+32)as*mut u64,value)},value);
  assert_eq!(unsafe{*libc::__error()},libc::EDOM);assert_eq!(unsafe{((base+32)as*const u64).read()},value);
  let mut after=unsafe{std::mem::zeroed()};unsafe{assert_eq!(libc::pthread_sigmask(libc::SIG_SETMASK,std::ptr::null(),&mut after),0);}
  for signal in 1..32{assert_eq!(unsafe{libc::sigismember(&before,signal)},unsafe{libc::sigismember(&after,signal)});}
  let bytes:Vec<u8>=(0..257).map(|n|(n*17)as u8).collect();unsafe{kl_native_writer_test_copy((base+128)as*mut _,bytes.as_ptr().cast(),bytes.len());}
  assert_eq!(unsafe{std::slice::from_raw_parts((base+128)as*const u8,bytes.len())},bytes);
  let mut registers:Registers=unsafe{std::mem::zeroed()};unsafe{native_store_probe((base+512)as*mut _,value,&mut registers);}
  assert_eq!(registers.before,registers.after);assert_eq!(registers.neon_before,registers.neon_after);assert_eq!(registers.flags_before,registers.flags_after);
  assert_eq!(unsafe{((base+512)as*const u64).read()},value);
 }
 #[test]
 fn foreign_c_and_background_writers_use_original_cpu_and_mapping_owner(){
  unsafe{libc::alarm(20);}
  super::super::signal::install_host_handlers();
  if let Ok(mode)=std::env::var("MACLAUNCHER_NATIVE_WRITER_CHILD"){
   if mode=="low-fd-exec"{let limit=libc::rlimit{rlim_cur:64,rlim_max:64};assert_eq!(unsafe{libc::setrlimit(libc::RLIMIT_NOFILE,&limit)},0);let _=mapping(RW|libc::PROT_EXEC);assert!(engine::descriptors_cloexec());return;}
   let base=if mode=="readonly"{mapping(libc::PROT_READ|libc::PROT_EXEC)}else{
    let memory=unsafe{libc::mmap(std::ptr::null_mut(),PAGE,libc::PROT_READ,libc::MAP_ANON|libc::MAP_PRIVATE,-1,0)};assert_ne!(memory,libc::MAP_FAILED);memory as u64
   };
   unsafe{kl_native_writer_test_store(base as*mut u64,99);}panic!("real protection failure was swallowed");
  }
  let foreground=mapping(RW|libc::PROT_EXEC);execute(foreground,false);
  let(mut native_thread,mut calling_thread)=(0u64,0u64);assert_eq!(unsafe{libc::pthread_threadid_np(0,&mut calling_thread)},0);
  unsafe{native_svc_retry((foreground+2048)as*mut u64,&mut native_thread);}
  assert_eq!(native_thread,calling_thread);assert_eq!(unsafe{((foreground+2048)as*const u64).read()},1);
  let conditional=(foreground+2064)as*mut u16;
  assert_eq!(unsafe{native_cas_retry16(conditional,0,1)},0);assert_eq!(unsafe{conditional.read()},1);
  assert_eq!(unsafe{native_cas_retry16(conditional,0,2)},1);assert_eq!(unsafe{conditional.read()},1);
  let background=mapping(RW|libc::PROT_EXEC);std::thread::spawn(move||execute(background,true)).join().unwrap();
  let shared=mapping(RW|libc::PROT_EXEC);
  let workers:Vec<_>=(0..8).map(|_|std::thread::spawn(move||{for _ in 0..8{unsafe{kl_native_writer_test_lrsc(shared as*mut u64);}}unsafe{kl_native_writer_test_add((shared+8)as*mut u64,8);}})).collect();
  for worker in workers{worker.join().unwrap();}
  assert_eq!(unsafe{(shared as*const u64).read()},64);assert_eq!(unsafe{((shared+8)as*const u64).read()},64);
  // Darwin kernel copyout reports EFAULT without a resumable CPU store fault.
  // The hardware-fault owner cannot grant this kernel access.
  let mut pipe=[-1;2];assert_eq!(unsafe{libc::pipe(pipe.as_mut_ptr())},0);
  let byte=0x5au8;assert_eq!(unsafe{libc::write(pipe[1],(&byte as*const u8).cast(),1)},1);
  assert_eq!(unsafe{libc::read(pipe[0],(foreground+1024)as*mut _,1)},-1);assert_eq!(unsafe{*libc::__error()},libc::EFAULT);
  assert_eq!(unsafe{((foreground+1024)as*const u8).read()},0);
  unsafe{libc::close(pipe[0]);libc::close(pipe[1]);}
  // A raw Darwin fork has no inherited helper thread. Decline before queueing;
  // fresh exec-based children initialize their own independent owner instead.
  let child=unsafe{libc::fork()};assert!(child>=0);
  if child==0{unsafe{kl_native_writer_test_store(shared as*mut u64,123);libc::_exit(90);}}
  let mut status=0;assert_eq!(unsafe{libc::waitpid(child,&mut status,0)},child);
  assert!(libc::WIFSIGNALED(status));assert!(matches!(libc::WTERMSIG(status),libc::SIGBUS|libc::SIGSEGV));
  for base in [foreground,background,shared]{assert_eq!(crate::patch::vm::region(base).unwrap().2,libc::PROT_READ);let _epoch=super::super::verity_pager::mutation();let _guard=memory::transaction();memory::forget(base,PAGE as u64);assert_eq!(unsafe{libc::munmap(base as*mut _,PAGE)},0);}
  for mode in ["unowned","readonly"]{
   let result=Command::new(std::env::current_exe().unwrap()).args(["--exact","sys::native_writer::tests::foreign_c_and_background_writers_use_original_cpu_and_mapping_owner","--nocapture"]).env("MACLAUNCHER_NATIVE_WRITER_CHILD",mode).output().unwrap();
   assert!(matches!(result.status.signal(),Some(libc::SIGBUS|libc::SIGSEGV)),"{mode}: {:?}\n{}",result.status,String::from_utf8_lossy(&result.stderr));
  }
  assert!(engine::descriptors_cloexec());
  let result=Command::new(std::env::current_exe().unwrap()).args(["--exact","sys::native_writer::tests::foreign_c_and_background_writers_use_original_cpu_and_mapping_owner","--nocapture"]).env("MACLAUNCHER_NATIVE_WRITER_CHILD","low-fd-exec").output().unwrap();
  assert!(result.status.success(),"low-fd-exec: {:?}\n{}",result.status,String::from_utf8_lossy(&result.stderr));
  unsafe{libc::alarm(0);}
 }
}
