use std::os::fd::{AsFd, AsRawFd, FromRawFd, OwnedFd};
use std::sync::atomic::{AtomicI32, Ordering};
use maclauncher_fence_completion_control::{pair, state, State};
static ERROR: AtomicI32 = AtomicI32::new(0);
fn fail<T>(error:i32,value:T)->T{ERROR.store(error,Ordering::Relaxed);value}
mod user_memory {
 pub fn read_exact(address:u64,length:usize)->Result<Vec<u8>,i32>{Ok(unsafe{std::slice::from_raw_parts(address as *const u8,length)}.to_vec())}
 pub fn write_exact(address:u64,bytes:&[u8])->Result<(),i32>{unsafe{std::ptr::copy_nonoverlapping(bytes.as_ptr(),address as *mut u8,bytes.len())};Ok(())}
}
// ACTUAL_IOCTL
mod syscall {
 pub use libc::poll;
 pub unsafe fn ioctl<T>(fd:i32,request:libc::c_ulong,info:*mut T)->i32 {
  assert_eq!(request,0xc038_3e04);assert_eq!(size_of::<super::linux_sync::FileInfo>(),56);
  if !maclauncher_fence_completion_control::is_sync_file(unsafe{std::os::fd::BorrowedFd::borrow_raw(fd)}) {return -1;}
  super::linux_sync::file_info(fd,info as u64) as i32
 }
}
// ACTUAL_GUEST
fn main(){
 let dpy=std::ptr::null_mut();
 let (file,writer)=pair().unwrap();let original=file.as_raw_fd();let sync=guest::insert(file);
 writer.signal(-5);
 let negative=unsafe{guest::client_wait(dpy,sync,0,||unreachable!())}==0&&ERROR.load(Ordering::Relaxed)==0x3003;
 let negative_server=unsafe{guest::eglWaitSyncKHR(dpy,sync,0)}==0&&ERROR.load(Ordering::Relaxed)==0x3003;
 let negative_server_core=unsafe{guest::eglWaitSync(dpy,sync,0)}==0&&ERROR.load(Ordering::Relaxed)==0x3003;
 let mut attribute=99;let negative_attribute=unsafe{guest::eglGetSyncAttribKHR(dpy,sync,0x30f1,&mut attribute)}==0&&attribute==99&&ERROR.load(Ordering::Relaxed)==0x3003;
 let mut attribute_core=99;let negative_attribute_core=unsafe{guest::eglGetSyncAttrib(dpy,sync,0x30f1,&mut attribute_core)}==0&&attribute_core==99&&ERROR.load(Ordering::Relaxed)==0x3003;
 let duplicate=unsafe{guest::eglDupNativeFenceFDANDROID(dpy,sync)};assert!(duplicate>=0);let duplicate=unsafe{OwnedFd::from_raw_fd(duplicate)};
 let duplicate_error=matches!(state(duplicate.as_fd()),State::Signaled{status:-5,..});
 assert_eq!(unsafe{guest::eglDestroySyncKHR(dpy,sync)},1);let original_closed=unsafe{libc::fcntl(original,libc::F_GETFD)}<0;
 let duplicate_preserved=matches!(state(duplicate.as_fd()),State::Signaled{status:-5,..});
 let (file,writer)=pair().unwrap();let sync=guest::insert(file);writer.signal(1);
 let success=unsafe{guest::client_wait(dpy,sync,0,||unreachable!())}==0x30f6;
 let server_success=unsafe{guest::eglWaitSyncKHR(dpy,sync,0)}==1&&unsafe{guest::eglWaitSync(dpy,sync,0)}==1;
 let mut attribute=0;let attribute_success=unsafe{guest::eglGetSyncAttribKHR(dpy,sync,0x30f1,&mut attribute)}==1&&attribute==0x30f2;
 assert_eq!(unsafe{guest::eglDestroySync(dpy,sync)},1);
 let (file,writer)=pair().unwrap();let sync=guest::insert(file);
 let timeout=unsafe{guest::client_wait(dpy,sync,0,||unreachable!())}==0x30f5;
 let mut attribute=99;let active_attribute=unsafe{guest::eglGetSyncAttribKHR(dpy,sync,0x30f1,&mut attribute)}==1&&attribute==0x30f3;
 drop(writer);
 let producer_loss=unsafe{guest::client_wait(dpy,sync,0,||unreachable!())}==0;
 assert_eq!(unsafe{guest::eglDestroySync(dpy,sync)},1);
 let forwarded=unsafe{guest::client_wait(dpy,1usize as *mut _,0,||12345)}==12345;
 let valid=negative&&negative_server&&negative_server_core&&negative_attribute&&negative_attribute_core&&duplicate_error&&original_closed&&duplicate_preserved&&success&&server_success&&attribute_success&&timeout&&active_attribute&&producer_loss&&forwarded;
 println!("negative={negative} server={negative_server}/{negative_server_core} attribute={negative_attribute}/{negative_attribute_core} duplicate={duplicate_error}/{duplicate_preserved} originalClosed={original_closed} success={success}/{server_success}/{attribute_success} timeout={timeout} active={active_attribute} producerLoss={producer_loss} forwarded={forwarded}");
 if !valid {std::process::exit(1)}
}
