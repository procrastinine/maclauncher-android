//! Trusted native Looper readiness lease. Original eventfd/timerfd notification
//! socketpairs retain their Linux counter/timer owner; the host only polls.
use super::{Kind,Pinned};
use crate::{context,errno::{self,Errno}};
use std::{ffi::c_void,os::fd::AsRawFd};
struct NativeErrno(i32);
impl NativeErrno{fn save()->Self{Self(unsafe{*libc::__error()})}}
impl Drop for NativeErrno{fn drop(&mut self){unsafe{*libc::__error()=self.0;}}}
#[derive(Clone,Copy)]
struct Owner{physical:i32,managed:i32}
// Both original Bionic pthreads and admitted Java pthreads enter through the
// original hostcall context. Confirm the registered physical thread as well as
// its canonical Android lease; a foreign-admission ticket is not required.
fn physical_thread(tid:i32)->bool{
 let mut physical=0u64;
 let status=unsafe{libc::pthread_threadid_np(0,&mut physical)};
 status==0 && physical!=0 && super::super::thread::host_thread_id(tid)==Some(physical)
}
impl Owner{
 fn current()->Result<Self,Errno>{
  let raw=context::current_ctx();
  if raw.is_null(){return Err(errno::EPERM);}
  let ctx=unsafe{&*raw};let thread=super::super::thread::current().ok_or(errno::EPERM)?;
  if ctx.in_host==0||ctx.callback_sp==0||context::guest_tp()==0||ctx.tid!=thread.tid as u64||crate::process_identity::thread()!=Some(Ok(thread.tid))||!physical_thread(thread.tid){return Err(errno::EPERM);}
  Ok(Self{physical:unsafe{libc::getpid()},managed:match crate::process_identity::process(){Some(Ok(id))=>id,_=>return Err(errno::EPERM)}})
 }
 fn verify(&self)->Result<(),Errno>{
  if unsafe{libc::getpid()}!=self.physical||crate::process_identity::process()!=Some(Ok(self.managed)){return Err(errno::EPERM);}Ok(())
 }
}
struct Lease{owner:Owner,pin:Pinned}
fn validate(pin:Pinned)->Result<Pinned,Errno>{
 let fd=pin.descriptor().as_raw_fd();
 if pin.fuse.is_some()||super::super::fuse_device::is_typed(fd){return Err(errno::EOPNOTSUPP);}
 let notification=matches!(pin.kind(),Some(Kind::Event(_)|Kind::Timer(_)));
 match pin.kind(){
  None=>{},
  Some(Kind::Event(_)|Kind::Timer(_))=>{},
  Some(Kind::Sock(socket)) if matches!(socket.ty,super::super::net::SockType::Stream)&&matches!(socket.family,super::super::net::Family::Unix|super::super::net::Family::Inet(_))=>{},
  Some(Kind::Path(_))=>return Err(errno::EBADF),
  Some(_)=>return Err(errno::EOPNOTSUPP),
 }
 let mut stat:libc::stat=unsafe{std::mem::zeroed()};
 if unsafe{libc::fstat(fd,&mut stat)}<0{return Err(errno::last());}
 if !matches!(stat.st_mode&libc::S_IFMT,libc::S_IFIFO|libc::S_IFSOCK){return Err(errno::EOPNOTSUPP);}
 let flags=unsafe{libc::fcntl(fd,libc::F_GETFL)};if flags<0{return Err(errno::last());}
 if flags&libc::O_EVTONLY!=0{return Err(errno::EBADF);}
 if stat.st_mode&libc::S_IFMT==libc::S_IFSOCK{
  if !notification&&!pin.socket_allowed(){return Err(errno::EOPNOTSUPP);}
  let mut ty=0i32;let mut length=std::mem::size_of_val(&ty)as libc::socklen_t;
  if unsafe{libc::getsockopt(fd,libc::SOL_SOCKET,libc::SO_TYPE,(&mut ty as *mut i32).cast(),&mut length)}<0{return Err(errno::last());}
  let expected=if notification{libc::SOCK_DGRAM}else{libc::SOCK_STREAM};
  if ty!=expected{return Err(errno::EOPNOTSUPP);}
 }
 Ok(pin)
}
#[repr(C)]pub struct Api{size:u32,version:u32,borrow:unsafe extern "C" fn(i32,*mut i32,*mut *mut c_void)->i64,release:unsafe extern "C" fn(*mut c_void)->i64}
const _:()={assert!(std::mem::size_of::<Api>()==24);};
unsafe extern "C" fn borrow(fd:i32,native:*mut i32,opaque:*mut *mut c_void)->i64{
 let _errno=NativeErrno::save();if native.is_null()||opaque.is_null(){return -(errno::EINVAL as i64);}
 unsafe{native.write(-1);opaque.write(std::ptr::null_mut());}
 let result=(||{let owner=Owner::current()?;let pin=validate(super::pin_guest(fd)?)?;let fd=pin.descriptor().as_raw_fd();Ok::<_,Errno>((fd,Box::into_raw(Box::new(Lease{owner,pin}))))})();
 match result{Ok((fd,lease))=>{unsafe{native.write(fd);opaque.write(lease.cast());}0},Err(e)=>-(e as i64)}
}
// This is a trusted host-owner capability, NEVER a guest packet argument. The C
// Looper owns the unique Box and exchanges its token to NULL before release.
// Runtime/process verification permits removal by another admitted Java thread
// or by the existing host Looper's thread-retirement owner; no guest TLS needed.
unsafe extern "C" fn release(opaque:*mut c_void)->i64{
 let _errno=NativeErrno::save();if opaque.is_null(){return -(errno::EINVAL as i64);}
 let lease=unsafe{&*opaque.cast::<Lease>()};if let Err(e)=lease.owner.verify(){return -(e as i64);}
 unsafe{drop(Box::from_raw(opaque.cast::<Lease>()));}0
}
static API:Api=Api{size:24,version:1,borrow,release};
#[unsafe(no_mangle)]pub extern "C" fn klj_bionic_poll_fd_api_v1()->*const Api{&API}
#[cfg(test)]mod tests{
 use super::*;use std::os::fd::{FromRawFd,OwnedFd};
 #[test]fn original_pin_native_poll_policy_and_close_retention(){
  const NAME:&str="sys::fdtab::native_poll_lease::tests::original_pin_native_poll_policy_and_close_retention";
  if super::super::isolated_kernel_test(NAME){return;}
  let(_guard,root)=crate::vfs::test_view();
  let mut out=7;let mut lease=7usize as *mut c_void;unsafe{*libc::__error()=libc::EACCES;}
  assert_eq!(unsafe{borrow(-1,&mut out,&mut lease)},-(errno::EPERM as i64));assert_eq!(out,-1);assert!(lease.is_null());assert_eq!(unsafe{*libc::__error()},libc::EACCES);
  let mut pipe=[0;2];assert_eq!(unsafe{libc::pipe(pipe.as_mut_ptr())},0);
  for fd in pipe{super::super::publish_guest(fd).unwrap();}
  let held=validate(super::super::pin_guest(pipe[0]).unwrap()).unwrap();let native=held.descriptor().as_raw_fd();assert_ne!(native,pipe[0]);assert!(super::super::is_hidden(native));assert!(super::super::TABLE.try_write().is_ok());
  super::super::withdraw_guest(pipe[0]).unwrap();super::super::on_close(pipe[0]);unsafe{libc::close(pipe[0]);}
  let byte=9u8;assert_eq!(unsafe{libc::write(pipe[1],(&byte as *const u8).cast(),1)},1);let mut p=libc::pollfd{fd:native,events:libc::POLLIN,revents:0};assert_eq!(unsafe{libc::poll(&mut p,1,0)},1);assert!(p.revents&libc::POLLIN!=0);
  let mut read=0u8;assert_eq!(unsafe{libc::read(native,(&mut read as *mut u8).cast(),1)},1);assert_eq!(read,9);
  // A retained Pinned is Send: legitimate host-owner cross-thread retirement.
  std::thread::spawn(move||drop(held)).join().unwrap();assert!(unsafe{libc::fcntl(native,libc::F_GETFD)}<0);
  super::super::withdraw_guest(pipe[1]).unwrap();super::super::on_close(pipe[1]);unsafe{libc::close(pipe[1]);}
  let mut pair=[0;2];assert_eq!(unsafe{libc::socketpair(libc::AF_UNIX,libc::SOCK_STREAM,0,pair.as_mut_ptr())},0);
  for fd in pair{super::super::publish_guest(fd).unwrap();}drop(validate(super::super::pin_guest(pair[0]).unwrap()).unwrap());
  for fd in pair{super::super::withdraw_guest(fd).unwrap();super::super::on_close(fd);unsafe{libc::close(fd);}}
  let p=root.join("data/looper-native-file");std::fs::write(&p,b"a").unwrap();let file=std::fs::File::open(p).unwrap();super::super::publish_guest(file.as_raw_fd()).unwrap();assert!(matches!(validate(super::super::pin_guest(file.as_raw_fd()).unwrap()),Err(errno::EOPNOTSUPP)));super::super::withdraw_guest(file.as_raw_fd()).unwrap();super::super::on_close(file.as_raw_fd());drop(file);
  let mut pair=[0;2];assert_eq!(unsafe{libc::socketpair(libc::AF_UNIX,libc::SOCK_DGRAM,0,pair.as_mut_ptr())},0);
  for fd in pair{super::super::publish_guest(fd).unwrap();}assert!(matches!(validate(super::super::pin_guest(pair[0]).unwrap()),Err(errno::EOPNOTSUPP)));
  for fd in pair{super::super::withdraw_guest(fd).unwrap();super::super::on_close(fd);drop(unsafe{OwnedFd::from_raw_fd(fd)});}
 }
 #[test]fn original_event_timer_readiness_counter_and_retirement(){
  const NAME:&str="sys::fdtab::native_poll_lease::tests::original_event_timer_readiness_counter_and_retirement";
  if super::super::isolated_kernel_test(NAME){return;}
  let(_guard,_root)=crate::vfs::test_view();
  let event=super::super::super::event::eventfd2([0,0o4000|0o2000000,0,0,0,0]);assert!(event>=0);
  let held=validate(super::super::pin_guest(event as i32).unwrap()).unwrap();let native=held.descriptor().as_raw_fd();
  assert_ne!(native,event as i32);assert!(super::super::is_hidden(native));assert!(super::super::TABLE.try_write().is_ok());
  let mut p=libc::pollfd{fd:native,events:libc::POLLIN,revents:0};assert_eq!(unsafe{libc::poll(&mut p,1,0)},0);
  let value=5u64;assert_eq!(super::super::super::event::write(event as i32,&value as*const u64 as u64,8),Some(8));assert_eq!(unsafe{libc::poll(&mut p,1,0)},1);
  let mut read=0u64;assert_eq!(super::super::super::event::read(event as i32,&mut read as*mut u64 as u64,8),Some(8));assert_eq!(read,5);assert_eq!(unsafe{libc::poll(&mut p,1,0)},0);
  let duplicate=super::super::super::fs::dup([event as u64,0,0,0,0,0]);assert!(duplicate>=0);assert_eq!(super::super::super::fs::close([event as u64,0,0,0,0,0]),0);
  let value=7u64;assert_eq!(super::super::super::event::write(duplicate as i32,&value as*const u64 as u64,8),Some(8));assert_eq!(unsafe{libc::poll(&mut p,1,0)},1);
  assert_eq!(super::super::super::event::read(duplicate as i32,&mut read as*mut u64 as u64,8),Some(8));assert_eq!(read,7);
  drop(held);assert_eq!(unsafe{libc::fcntl(native,libc::F_GETFD)},-1);assert_eq!(super::super::super::fs::close([duplicate as u64,0,0,0,0,0]),0);
  let timer=super::super::super::event::timerfd_create([1,0o4000|0o2000000,0,0,0,0]);assert!(timer>=0);
  let held=validate(super::super::pin_guest(timer as i32).unwrap()).unwrap();p.fd=held.descriptor().as_raw_fd();
  let arm=[0i64,0,0,5_000_000];assert_eq!(super::super::super::event::timerfd_settime([timer as u64,0,arm.as_ptr()as u64,0,0,0]),0);assert_eq!(unsafe{libc::poll(&mut p,1,500)},1);
  assert_eq!(super::super::super::event::read(timer as i32,&mut read as*mut u64 as u64,8),Some(8));assert!(read>=1);
  assert_eq!(super::super::super::fs::close([timer as u64,0,0,0,0,0]),0);drop(held);
 }

}
