//! One datagram's header, full wire length and consume share its socket receipt.
//! Readiness waits hold no receive admission or descriptor lifecycle lock.
use super::{SocketInput,Sock,SockType,Family,Received,FRAME,DGRAM_MAGIC,L_MSG_TRUNC,L_MSG_DONTWAIT};
use crate::errno::{self,Errno};
use aim_storage::private_fd::PrivateFile;
use std::{collections::HashMap,fs::{File,OpenOptions},os::{fd::{AsRawFd,FromRawFd},unix::fs::{OpenOptionsExt,MetadataExt}},sync::{Arc,Mutex,MutexGuard,TryLockError,Weak,LazyLock},time::{Duration,Instant}};

fn io(error:std::io::Error)->Errno{errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))}
pub(super) struct Admission{file:PrivateFile,local:Mutex<()>,inherited_fd:i32}
// There is exactly one descriptor for this dedicated lock inode in a process.
// Closing any descriptor for a POSIX-lock inode releases all of that process's
// locks. Local SCM aliases therefore reuse this owner without opening a file.
static OWNERS:LazyLock<Mutex<HashMap<String,Weak<Admission>>>>=LazyLock::new(||Mutex::new(HashMap::new()));
struct Held<'a>{owner:&'a Admission,_local:MutexGuard<'a,()>}
fn lock(fd:i32,kind:i16)->Result<(),Errno>{let mut value:libc::flock=unsafe{std::mem::zeroed()};value.l_type=kind;value.l_whence=libc::SEEK_SET as i16;if unsafe{libc::fcntl(fd,libc::F_SETLK,&value)}<0{Err(errno::last())}else{Ok(())}}
impl Drop for Held<'_>{fn drop(&mut self){let _=lock(self.owner.file.as_raw_fd(),libc::F_UNLCK);}}
impl AsRawFd for Admission{fn as_raw_fd(&self)->i32{self.file.as_raw_fd()}}
impl Admission{
 fn try_enter(&self)->Result<Option<Held<'_>>,Errno>{
  let local=match self.local.try_lock(){Ok(guard)=>guard,Err(TryLockError::WouldBlock)=>return Ok(None),Err(TryLockError::Poisoned(error))=>error.into_inner()};
  match lock(self.file.as_raw_fd(),libc::F_WRLCK){Ok(())=>Ok(Some(Held{owner:self,_local:local})),Err(errno::EAGAIN|errno::EACCES)=>Ok(None),Err(error)=>Err(error)}
 }
}
/// Allocate before a later NOFILE reduction; aliases retain the same owner.
/// A fork child inherits this inode descriptor and uses its own PID lock owner.
pub(super) fn initialize(fd:i32,socket:&Arc<Sock>)->Result<(),Errno>{
 if socket.ty!=SockType::Dgram||!matches!(socket.family,Family::Unix|Family::Inet(_)){return Ok(());}
 let mut owner=socket.datagram_receive.lock().unwrap();
 if owner.is_some(){return Ok(());}
 let Some(receipt)=*socket.inode_allocation.lock().unwrap()else{return Ok(());};
 receipt.validate(fd).map_err(io)?;
 let mut owners=OWNERS.lock().unwrap();let key=receipt.name();
 if let Some(existing)=owners.get(&key).and_then(Weak::upgrade){*owner=Some(existing);return Ok(());}
 let directory=super::socket_inode::directory();std::fs::create_dir_all(&directory).map_err(io)?;
 let file=PrivateFile::allocate(||OpenOptions::new().read(true).write(true).create(true).truncate(false)
  .mode(0o600).custom_flags(libc::O_NOFOLLOW|libc::O_CLOEXEC).open(directory.join(receipt.name()).with_extension("receive"))).map_err(io)?;
 let admission=Arc::new(Admission{inherited_fd:file.as_raw_fd(),file,local:Mutex::new(())});
 if owners.len()>1024{owners.retain(|_,owner|owner.strong_count()!=0);}
 owners.insert(key,Arc::downgrade(&admission));*owner=Some(admission);Ok(())
}

pub(super) fn save(socket:&Arc<Sock>,writer:&mut super::super::fork_state::Writer){
 let owner=socket.datagram_receive.lock().unwrap().clone();
 if let Some(owner)=owner{let fd=owner.as_raw_fd();writer.retain_owner(fd,super::super::fdtab::Kind::Sock(socket.clone()));writer.i32(fd);}else{writer.i32(-1);}
}
pub(super) fn restore(socket:&Arc<Sock>,fd:i32)->Result<(),Errno>{
 if fd<0{return Ok(());}
 let receipt=socket.inode_allocation.lock().unwrap().ok_or(errno::EIO)?;
 let key=receipt.name();let mut owners=OWNERS.lock().unwrap();
 if let Some(existing)=owners.get(&key).and_then(Weak::upgrade){
  if existing.inherited_fd!=fd{return Err(errno::EIO);}
  *socket.datagram_receive.lock().unwrap()=Some(existing);return Ok(());
 }
 // Only the runtime-owned fork receipt supplies this hidden descriptor. Check
 // its exact dedicated inode before adopting it; it is never a guest FD claim.
 let mut actual:libc::stat=unsafe{std::mem::zeroed()};if unsafe{libc::fstat(fd,&mut actual)}<0{return Err(errno::last());}
 let expected=std::fs::symlink_metadata(super::socket_inode::directory().join(&key).with_extension("receive")).map_err(io)?;
 if !expected.is_file()||actual.st_dev as u64!=expected.dev()||actual.st_ino!=expected.ino(){return Err(errno::EIO);}
 let file=PrivateFile::adopt(unsafe{File::from_raw_fd(fd)}).map_err(io)?;
 let admission=Arc::new(Admission{file,local:Mutex::new(()),inherited_fd:fd});
 owners.insert(key,Arc::downgrade(&admission));*socket.datagram_receive.lock().unwrap()=Some(admission);Ok(())
}

struct Wait{nonblocking:bool,deadline:Option<Instant>}
unsafe extern "C"{#[link_name="select$DARWIN_EXTSN"]fn select_extended(n:i32,read:*mut libc::fd_set,write:*mut libc::fd_set,except:*mut libc::fd_set,timeout:*mut libc::timeval)->i32;}
impl Wait{
 fn capture(fd:i32,flags:u64)->Result<Self,Errno>{
  let status=unsafe{libc::fcntl(fd,libc::F_GETFL)};if status<0{return Err(errno::last());}
  let nonblocking=flags&L_MSG_DONTWAIT!=0||status&libc::O_NONBLOCK!=0;
  let mut timeout:libc::timeval=unsafe{std::mem::zeroed()};let mut length=std::mem::size_of_val(&timeout)as u32;
  if unsafe{libc::getsockopt(fd,libc::SOL_SOCKET,libc::SO_RCVTIMEO,(&mut timeout as*mut libc::timeval).cast(),&mut length)}<0{return Err(errno::last());}
  let duration=Duration::from_secs(timeout.tv_sec.max(0)as u64)+Duration::from_micros(timeout.tv_usec.max(0)as u64);
  Ok(Self{nonblocking,deadline:(!duration.is_zero()).then(||Instant::now()+duration)})
 }
 fn wait(&self,fd:i32,contended:bool)->Result<(),Errno>{
  if self.nonblocking{return Err(errno::EAGAIN);}
  let milliseconds=match self.deadline{
   Some(deadline)=>{let remaining=deadline.saturating_duration_since(Instant::now());if remaining.is_zero(){return Err(errno::EAGAIN);}remaining.as_millis().clamp(1,i32::MAX as u128)as i32},
   None=>-1,
  };
  // A held admission contains only nonblocking kernel inspection/receive.
  // Do not queue behind it with an uninterruptible Rust mutex or flock wait.
  let result=if contended{unsafe{libc::poll(std::ptr::null_mut(),0,if milliseconds<0{1}else{milliseconds.min(1)})}}else{
   // Darwin poll(nfds=1) rejects even an existing FD at NOFILE0. The
   // unlimited-select entry accepts existing high descriptors without an
   // allocation or any change to the caller's descriptor limit.
   let mut readable=vec![0u32;fd as usize/32+1];readable[fd as usize/32]|=1u32<<(fd as usize%32);
   let mut timeout=libc::timeval{tv_sec:0,tv_usec:0};let timeout_pointer=if let Some(deadline)=self.deadline{let remaining=deadline.saturating_duration_since(Instant::now());timeout.tv_sec=remaining.as_secs()as _;timeout.tv_usec=remaining.subsec_micros()as _;&mut timeout}else{std::ptr::null_mut()};
   unsafe{select_extended(fd+1,readable.as_mut_ptr().cast(),std::ptr::null_mut(),std::ptr::null_mut(),timeout_pointer)}
  };
  if result<0{return Err(errno::last());}
  if self.deadline.is_some_and(|deadline|Instant::now()>=deadline){return Err(errno::EAGAIN);}
  Ok(())
 }
}

pub(super) fn receive(input:&SocketInput,iov:&[libc::iovec],want_name:bool,ctrl_cap:usize,flags:u64)->Result<Received,i64>{
 let Some(socket)=super::any_sock(input)else{return super::recv_original(input,iov,want_name,ctrl_cap,flags);};
 if socket.ty!=SockType::Dgram||!matches!(socket.family,Family::Unix|Family::Inet(_))
  ||matches!(&socket.family,Family::Inet(options)if options.icmp4.load(std::sync::atomic::Ordering::Relaxed)){
  return super::recv_original(input,iov,want_name,ctrl_cap,flags);
 }
 initialize(input.fd(),&socket).map_err(|error|-(error as i64))?;
 let Some(owner)=socket.datagram_receive.lock().unwrap().clone()else{return super::recv_original(input,iov,want_name,ctrl_cap,flags);};
 let wait=Wait::capture(input.fd(),flags).map_err(|error|-(error as i64))?;
 loop{
  let mut contended=true;
  if let Some(_admission)=owner.try_enter().map_err(|error|-(error as i64))?{
   let mut header=[0u8;FRAME];let mut vector=[libc::iovec{iov_base:header.as_mut_ptr().cast(),iov_len:FRAME}];
   let peek=super::recvmsg_host(input.fd(),&mut vector,&mut [],false,super::host_recv_flags(flags)|libc::MSG_PEEK|libc::MSG_DONTWAIT);
   match peek{
    Ok((observed,..))=>{
     let mut wire=0i32;let mut length=4u32;
     if unsafe{libc::getsockopt(input.fd(),libc::SOL_SOCKET,libc::SO_NREAD,(&mut wire as*mut i32).cast(),&mut length)}<0{return Err(-(errno::last()as i64));}
     if wire<0||(wire as usize)<observed{return Err(-(errno::EIO as i64));}
     let framed=matches!(socket.family,Family::Unix)&&observed>=FRAME&&super::parse_header(&header).0==DGRAM_MAGIC;
     match super::recv_original(input,iov,want_name,ctrl_cap,flags|L_MSG_DONTWAIT){
      Ok(mut received)=>{if flags&L_MSG_TRUNC!=0{received.n=(wire as usize-if framed{FRAME}else{0})as i64;}return Ok(received);},
      Err(error)if error==-(errno::EAGAIN as i64)=>contended=false,
      Err(error)=>return Err(error),
     }
    },
    Err(error)if error==-(errno::EAGAIN as i64)=>contended=false,
    Err(error)=>return Err(error),
   }
  }
  wait.wait(input.fd(),contended).map_err(|error|-(error as i64))?;
 }
}

#[cfg(test)]mod tests{
 use super::*;use std::{os::{fd::IntoRawFd,unix::net::UnixDatagram},process::Command};
 #[test]#[ignore="subprocess tests the actual other-PID lock owner"]
 fn competing_pid(){let path=std::env::args().find_map(|arg|arg.strip_prefix("DGRAM_LOCK=").map(str::to_owned)).unwrap();let file=OpenOptions::new().read(true).write(true).open(path).unwrap();assert!(matches!(lock(file.as_raw_fd(),libc::F_WRLCK),Err(errno::EAGAIN|errno::EACCES)));println!("ACTUAL_PID_ADMISSION_HELD");}
 #[test]
 fn scm_alias_and_fork_carrier_retain_one_pid_lock(){
  use super::super::super::fdtab::{self,Kind};
  const NAME:&str="sys::net::datagram_receive::tests::scm_alias_and_fork_carrier_retain_one_pid_lock";
  if fdtab::isolated_kernel_test(NAME){return;}
  let(a,b)=UnixDatagram::pair().unwrap();let fd=a.into_raw_fd();let socket=Sock::of(SockType::Dgram,Family::Unix);
  fdtab::insert(fd,Kind::Sock(socket.clone()));fdtab::publish_guest(fd).unwrap();super::super::socket_inode::allocated(fd).unwrap();
  let receipt=socket.inode_allocation.lock().unwrap().unwrap();let owner=socket.datagram_receive.lock().unwrap().clone().unwrap();
  // A native unframed sender has no private header to subtract.
  assert_eq!(b.send(b"unframed").unwrap(),8);let input=super::super::SocketInput::capture(fd).unwrap();let mut prefix=[0u8;3];let vectors=[libc::iovec{iov_base:prefix.as_mut_ptr().cast(),iov_len:3}];let received=receive(&input,&vectors,false,0,L_MSG_TRUNC).unwrap();assert_eq!(received.n,8);assert_eq!(&prefix,b"unf");assert_ne!(received.flags&libc::MSG_TRUNC,0);drop(input);
  let held=owner.try_enter().unwrap().unwrap();
  // Real SCM_RIGHTS carries only the socket capability. Its separately
  // validated receipt joins the existing local admission without opening or
  // closing another descriptor for the dedicated POSIX-lock inode.
  let mut control=[0u64;4];let mut byte=1u8;let mut vector=libc::iovec{iov_base:(&mut byte as*mut u8).cast(),iov_len:1};let mut message:libc::msghdr=unsafe{std::mem::zeroed()};message.msg_iov=&mut vector;message.msg_iovlen=1;message.msg_control=control.as_mut_ptr().cast();message.msg_controllen=unsafe{libc::CMSG_SPACE(4)};
  unsafe{let header=libc::CMSG_FIRSTHDR(&message);(*header).cmsg_level=libc::SOL_SOCKET;(*header).cmsg_type=libc::SCM_RIGHTS;(*header).cmsg_len=libc::CMSG_LEN(4);std::ptr::write_unaligned(libc::CMSG_DATA(header).cast::<i32>(),fd);assert_eq!(libc::sendmsg(fd,&message,0),1);assert_eq!(libc::recvmsg(b.as_raw_fd(),&mut message,0),1);}
  let alias=unsafe{std::ptr::read_unaligned(libc::CMSG_DATA(libc::CMSG_FIRSTHDR(&message)).cast::<i32>())};
  let other=Sock::of(SockType::Dgram,Family::Unix);fdtab::insert(alias,Kind::Sock(other.clone()));fdtab::publish_guest(alias).unwrap();super::super::socket_inode::install(alias,receipt).unwrap();
  assert!(Arc::ptr_eq(&owner,other.datagram_receive.lock().unwrap().as_ref().unwrap()));
  fdtab::withdraw_guest(alias).unwrap();fdtab::on_close(alias);assert_eq!(unsafe{libc::close(alias)},0);drop(other);
  let mut writer=super::super::super::fork_state::Writer::default();save(&socket,&mut writer);let carriers=writer.take_private().unwrap();assert_eq!(carriers.len(),1);assert_eq!(carriers[0].source().as_raw_fd(),owner.as_raw_fd());drop(carriers);
  let output=Command::new(std::env::current_exe().unwrap()).args(["--exact","sys::net::datagram_receive::tests::competing_pid","--ignored","--nocapture","--skip",&format!("DGRAM_LOCK={}",super::super::socket_inode::directory().join(receipt.name()).with_extension("receive").display())]).output().unwrap();assert!(output.status.success(),"{} {}",String::from_utf8_lossy(&output.stdout),String::from_utf8_lossy(&output.stderr));assert!(String::from_utf8_lossy(&output.stdout).contains("ACTUAL_PID_ADMISSION_HELD"));
  drop(held);fdtab::withdraw_guest(fd).unwrap();fdtab::on_close(fd);assert_eq!(unsafe{libc::close(fd)},0);
 }
}
