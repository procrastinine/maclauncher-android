//! Thin binding to the same kernel-leased process/thread owner as the JVM.
//! AIM retains its existing thread records, signals, futexes and credentials;
//! identifiers are leases, never encodings of a Darwin PID or thread token.
use std::ffi::{CStr,CString,c_char,c_int,c_void};
use std::sync::OnceLock;

type Configure=unsafe extern "C" fn(*const c_char)->c_int;
type Current=unsafe extern "C" fn()->c_int;
type Adopt=unsafe extern "C" fn(c_int)->c_int;
type Release=unsafe extern "C" fn(c_int);
type Owner=unsafe extern "C" fn(c_int,*mut c_int,*mut c_int,*mut c_int)->c_int;
type Pair=unsafe extern "C" fn(c_int,c_int)->c_int;
#[repr(C)]
struct Descriptors {
 enter:unsafe extern "C" fn()->*mut c_void,
 adopt:unsafe extern "C" fn(c_int)->c_int,
 closed:unsafe extern "C" fn(c_int),
 leave:unsafe extern "C" fn(*mut c_void),
}
unsafe extern "C" fn descriptors_enter()->*mut c_void{
 Box::into_raw(Box::new(crate::sys::fdtab::lifecycle())).cast()
}
unsafe extern "C" fn descriptors_adopt(fd:c_int)->c_int{
 use std::os::fd::{FromRawFd,IntoRawFd};
 // The C allocation has not been published or locked yet. Consume it using
 // the same allocator as every other AIM-private storage descriptor. Once
 // returned, the final handle stays unchanged for the kernel lease lifetime.
 match aim_storage::private_fd::Registrar::adopt(
  &crate::sys::fdtab::StorageRegistrar,unsafe{std::os::fd::OwnedFd::from_raw_fd(fd)}){
  Ok(descriptor)=>descriptor.into_raw_fd(),
  Err(error)=>{unsafe{*libc::__error()=error.raw_os_error().unwrap_or(libc::EIO);} -1}
 }
}
unsafe extern "C" fn descriptors_closed(fd:c_int){
 crate::sys::fdtab::unhide(fd);crate::sys::fd_visibility::private_closed(fd);
}
unsafe extern "C" fn descriptors_leave(guard:*mut c_void){
 unsafe{drop(Box::from_raw(guard.cast::<crate::sys::fdtab::Lifecycle>()));}
}
static DESCRIPTORS:Descriptors=Descriptors{
 enter:descriptors_enter,adopt:descriptors_adopt,closed:descriptors_closed,leave:descriptors_leave,
};
struct Api {
 directory:CString,process:Current,thread:Current,reserve:Current,adopt:Adopt,
 release:Release,owner:Owner,from_host:Adopt,child:Adopt,child_host:Adopt,retain_child:Adopt,reap_child:Adopt,
 getpgid:Adopt,getsid:Adopt,setpgid:Pair,setsid:Current,
 get_dumpable:Current,set_dumpable:Adopt,dumpable_of:Adopt,
}
static API:OnceLock<Api>=OnceLock::new();

/// Install before guest entry. A stock AIM invocation without a selected
/// identity directory retains its original owner and execution behavior.
pub unsafe fn install(handle:*mut c_void)->Result<(),String>{
 use std::os::unix::ffi::OsStrExt;
 let Some(directory)=std::env::var_os("KL_ANDROID_ID_DIRECTORY")else{return Ok(());};
 let directory=CString::new(directory.as_bytes()).map_err(|error|error.to_string())?;
 unsafe fn address(handle:*mut c_void,name:&CStr)->Result<*mut c_void,String>{
  let value=unsafe{libc::dlsym(handle,name.as_ptr())};
  if value.is_null(){Err(format!("Android identity operation is unavailable: {}",name.to_string_lossy()))}else{Ok(value)}
 }
 // Each pointer is the original C declaration; no identifier state is cached
 // here independently of the native owner.
 let configure:Configure=unsafe{std::mem::transmute(address(handle,c"klj_process_ids_configure")?)};
 let descriptors:unsafe extern "C" fn(*const Descriptors)->c_int=unsafe{
  std::mem::transmute(address(handle,c"klj_process_descriptors_owned")?)
 };
 if unsafe{descriptors(&DESCRIPTORS)}<0{return Err(std::io::Error::last_os_error().to_string());}
 let api=Api{directory,
  process:unsafe{std::mem::transmute(address(handle,c"klj_process_id")?)},
  thread:unsafe{std::mem::transmute(address(handle,c"klj_thread_id")?)},
  reserve:unsafe{std::mem::transmute(address(handle,c"klj_thread_id_reserve")?)},
  adopt:unsafe{std::mem::transmute(address(handle,c"klj_thread_id_adopt")?)},
  release:unsafe{std::mem::transmute(address(handle,c"klj_thread_id_release")?)},
  owner:unsafe{std::mem::transmute(address(handle,c"klj_process_id_query")?)},
  from_host:unsafe{std::mem::transmute(address(handle,c"klj_process_id_from_host")?)},
  child:unsafe{std::mem::transmute(address(handle,c"klj_process_child_id")?)},
  child_host:unsafe{std::mem::transmute(address(handle,c"klj_process_child_host")?)},
  retain_child:unsafe{std::mem::transmute(address(handle,c"klj_process_child_retain")?)},
  reap_child:unsafe{std::mem::transmute(address(handle,c"klj_process_child_reaped")?)},
  getpgid:unsafe{std::mem::transmute(address(handle,c"klj_process_getpgid")?)},
  getsid:unsafe{std::mem::transmute(address(handle,c"klj_process_getsid")?)},
  setpgid:unsafe{std::mem::transmute(address(handle,c"klj_process_setpgid")?)},
  setsid:unsafe{std::mem::transmute(address(handle,c"klj_process_setsid")?)},
  get_dumpable:unsafe{std::mem::transmute(address(handle,c"klj_process_get_dumpable")?)},
  set_dumpable:unsafe{std::mem::transmute(address(handle,c"klj_process_set_dumpable")?)},
  dumpable_of:unsafe{std::mem::transmute(address(handle,c"klj_process_dumpable_of")?)},
 };
 if unsafe{configure(api.directory.as_ptr())}<0{return Err(std::io::Error::last_os_error().to_string());}
 aim_storage::posix_broker::set_pid_resolver(api.from_host)?;
 API.set(api).map_err(|_|"Android identity owner is already installed".to_owned())
}
pub fn enabled()->bool{API.get().is_some()}
fn checked(value:c_int)->Result<i32,i64>{if value>0{Ok(value)}else{Err(-(crate::errno::last() as i64))}}
pub fn process()->Option<Result<i32,i64>>{API.get().map(|api|checked(unsafe{(api.process)()}))}
pub fn thread()->Option<Result<i32,i64>>{API.get().map(|api|checked(unsafe{(api.thread)()}))}
pub fn reserve()->Option<Result<i32,i64>>{API.get().map(|api|checked(unsafe{(api.reserve)()}))}
pub fn adopt(id:i32)->Result<(),i64>{if let Some(api)=API.get(){checked(unsafe{(api.adopt)(id)})?;}Ok(())}
pub fn release(id:i32){if let Some(api)=API.get(){unsafe{(api.release)(id)};}}
pub fn from_host(host:i32)->Option<Result<i32,i64>>{
 API.get().map(|api|if host==0{Ok(0)}else{
  let child=unsafe{(api.child)(host)};
  if child>0{Ok(child)}else{checked(unsafe{(api.from_host)(host)})}
 })
}
pub fn retain_child(host:i32)->Result<(),i64>{
 if let Some(api)=API.get(){checked(unsafe{(api.retain_child)(host)})?;}Ok(())
}
pub fn child_host(id:i32)->Option<i32>{
 API.get().and_then(|api|{let host=unsafe{(api.child_host)(id)};(host>0).then_some(host)})
}
pub fn reap_child(host:i32){
 if let Some(api)=API.get(){if unsafe{(api.child)(host)}>0{unsafe{(api.reap_child)(host)};}}
}
fn native_result(value:i32)->i64{if value>=0{value as i64}else{-(crate::errno::last()as i64)}}
pub fn getpgid(id:i32)->Option<i64>{API.get().map(|api|native_result(unsafe{(api.getpgid)(id)}))}
pub fn getsid(id:i32)->Option<i64>{API.get().map(|api|native_result(unsafe{(api.getsid)(id)}))}
pub fn setpgid(id:i32,group:i32)->Option<i64>{API.get().map(|api|native_result(unsafe{(api.setpgid)(id,group)}))}
pub fn setsid()->Option<i64>{API.get().map(|api|native_result(unsafe{(api.setsid)()}))}
#[derive(Clone,Copy)]
pub struct Identity{pub host:i32,pub kind:i32,pub process:i32}
pub fn owner(id:i32)->Option<Result<Identity,i64>>{
 API.get().map(|api|{
  let mut result=Identity{host:0,kind:0,process:0};
  let status=unsafe{(api.owner)(id,&mut result.host,&mut result.kind,&mut result.process)};
  if status==0{Ok(result)}else{Err(-(crate::errno::last() as i64))}
 })
}

/// Shared policy only; obtaining a host task port remains a separate check.
pub fn get_dumpable()->Option<i64>{API.get().map(|api|native_result(unsafe{(api.get_dumpable)()}))}
pub fn set_dumpable(value:i32)->Option<i64>{API.get().map(|api|native_result(unsafe{(api.set_dumpable)(value)}))}
pub fn dumpable_of_host(host:i32)->Option<bool>{API.get().map(|api|{
 let id=unsafe{(api.from_host)(host)};id>0&&unsafe{(api.dumpable_of)(id)}==1
})}
