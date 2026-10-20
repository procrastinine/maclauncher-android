//! Linux open descriptions retain the namespace through which they were opened.
//! Darwin supplies the current vnode name; a retained mount root supplies the
//! corresponding Linux name without selecting an unrelated bind alias.
use crate::{errno::{self,Errno},vfs};
use aim_storage::private_fd::PrivateFd;
use std::{ffi::{CString,OsStr},os::{fd::{AsRawFd,FromRawFd},unix::ffi::OsStrExt},path::{Path,PathBuf},sync::Arc};

pub struct OpenedPath { root:Option<PrivateFd>, guest:String, initial:String }
fn io(error:std::io::Error)->Errno{errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))}
fn fd_path(fd:i32)->Result<PathBuf,Errno>{
 let mut bytes=[0u8;libc::PATH_MAX as usize];
 if unsafe{libc::fcntl(fd,libc::F_GETPATH,bytes.as_mut_ptr())}<0{return Err(errno::last());}
 let length=bytes.iter().position(|byte|*byte==0).ok_or(36)?;
 Ok(PathBuf::from(OsStr::from_bytes(&bytes[..length])))
}
impl OpenedPath {
 pub fn capture(fd:i32,resolved:&vfs::Resolved)->Result<Arc<Self>,Errno>{
  // The shared width adapter publishes its maps snapshot through an actual
  // file. Its per-inode origin uses AIM's existing Linux user-xattr spelling.
  let mut origin=[0u8;96];
  let length=unsafe{libc::fgetxattr(fd,c"dev.aim.xattr.user.maclauncher.proc-origin".as_ptr(),origin.as_mut_ptr().cast(),origin.len(),0,0)};
  if length>=0 {
   let origin=std::str::from_utf8(&origin[..length as usize]).map_err(|_|errno::EINVAL)?;
   if !origin.starts_with("/proc/")||!origin.ends_with("/maps"){return Err(errno::EINVAL);}
   return Self::virtual_node(origin.to_owned());
  }
  // Read-only image names are immutable Linux dentries. Darwin F_GETPATH may
  // return another hardlink to the same vnode (including another APEX), which
  // is not the dentry this open resolved. Keep the original canonical guest
  // name in the existing open-description owner instead of reversing a vnode.
  if resolved.read_only(){return Self::virtual_node(resolved.guest.clone());}
  let(guest,host)=vfs::opened_mount(&resolved.guest);
  let host=CString::new(host.as_os_str().as_bytes()).map_err(|_|errno::EINVAL)?;
  let root=PrivateFd::allocate(||{
   let fd=unsafe{libc::open(host.as_ptr(),libc::O_EVTONLY|libc::O_CLOEXEC)};
   if fd<0{Err(std::io::Error::last_os_error())}else{Ok(unsafe{std::os::fd::OwnedFd::from_raw_fd(fd)})}
  }).map_err(io)?;
  Ok(Arc::new(Self{root:Some(root),guest,initial:resolved.guest.clone()}))
 }
 /// A virtual filesystem node owns its Linux name, independently of the
 /// unlinked kernel file used to carry its bytes.
 pub fn virtual_node(guest:String)->Result<Arc<Self>,Errno>{
  if !guest.starts_with('/')||guest.as_bytes().contains(&0){return Err(errno::EINVAL);}
  Ok(Arc::new(Self{root:None,initial:guest.clone(),guest}))
 }
 pub fn virtual_name(&self)->Option<&str>{self.root.is_none().then_some(self.guest.as_str())}
 pub fn path(&self,fd:i32)->Result<String,Errno>{
  let Some(root)=&self.root else{return Ok(self.guest.clone());};
  let actual=match fd_path(fd){Ok(path)=>path,Err(error)=>{
   let mut stat:libc::stat=unsafe{std::mem::zeroed()};
   if unsafe{libc::fstat(fd,&mut stat)}==0&&stat.st_nlink==0{return Ok(format!("{} (deleted)",self.initial));}
   return Err(error);
  }};
  let root=fd_path(root.as_raw_fd())?;
  let relative=actual.strip_prefix(&root).map_err(|_|errno::EXDEV)?;
  Ok(if relative==Path::new(""){self.guest.clone()}else{format!("{}/{}",self.guest.trim_end_matches('/'),relative.display())})
 }
 pub fn save(&self,w:&mut super::super::fork_state::Writer){let fd=self.descriptor();if fd>=0{w.retain_private(fd);}w.i32(fd);w.str(&self.guest);w.str(&self.initial);}
 pub fn restore(r:&mut super::super::fork_state::Reader)->Result<(Arc<Self>,i32),Errno>{
  let fd=r.i32();let guest=r.str();let initial=r.str();
  Ok((Self::from_inherited(fd,guest,initial)?,fd))
 }
 pub fn from_inherited(fd:i32,guest:String,initial:String)->Result<Arc<Self>,Errno>{
  if !guest.starts_with('/')||!initial.starts_with('/'){return Err(errno::EINVAL);}
  if fd==-1{return Self::virtual_node(guest);}
  let root=PrivateFd::allocate(||{let copy=unsafe{libc::fcntl(fd,libc::F_DUPFD_CLOEXEC,0)};if copy<0{Err(std::io::Error::last_os_error())}else{Ok(unsafe{std::os::fd::OwnedFd::from_raw_fd(copy)})}}).map_err(io)?;
  Ok(Arc::new(Self{root:Some(root),guest,initial}))
 }
 pub fn descriptor(&self)->i32{self.root.as_ref().map_or(-1,AsRawFd::as_raw_fd)}
 pub fn exec_fields(&self)->(String,String){
  let hex=|text:&str|text.as_bytes().iter().map(|byte|format!("{byte:02x}")).collect::<String>();
  (hex(&self.guest),hex(&self.initial))
 }
 pub fn exec_path(text:&str)->Result<String,Errno>{
  if text.len()%2!=0||!text.is_ascii(){return Err(errno::EINVAL);}
  let bytes=(0..text.len()).step_by(2).map(|at|u8::from_str_radix(&text[at..at+2],16).map_err(|_|errno::EINVAL)).collect::<Result<Vec<_>,_>>()?;
  String::from_utf8(bytes).map_err(|_|errno::EINVAL)
 }
}
