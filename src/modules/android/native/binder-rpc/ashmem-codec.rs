#[path="original/crates/aim-ashmem/src/lib.rs"] mod original;
pub use original::MARK;
#[path="ashmem-access.rs"] pub mod capability;
use std::{ffi::CStr,io};

fn result(operation:impl FnOnce()->io::Result<()>)->i32{
 match operation(){Ok(())=>0,Err(error)=>-error.raw_os_error().unwrap_or(libc::EIO)}
}
fn state(fd:i32)->io::Result<original::State>{
 let mut stat:libc::stat=unsafe{std::mem::zeroed()};
 if unsafe{libc::fstat(fd,&mut stat)}<0{return Err(io::Error::last_os_error());}
 if stat.st_mode&libc::S_IFMT!=libc::S_IFREG||stat.st_nlink!=0||stat.st_flags&original::MARK!=original::MARK{return Err(io::Error::from_raw_os_error(libc::ENOTTY));}
 let mut bytes=vec![0u8;4096];
 let size=unsafe{libc::fgetxattr(fd,original::XATTR.as_ptr(),bytes.as_mut_ptr().cast(),bytes.len(),0,0)};
 if size<0{return Err(io::Error::last_os_error());}
 original::State::decode(&bytes[..size as usize]).ok_or_else(||io::Error::from_raw_os_error(libc::EINVAL))
}
fn store(fd:i32,state:&original::State)->io::Result<()>{
 let bytes=state.encode();
 if unsafe{libc::fsetxattr(fd,original::XATTR.as_ptr(),bytes.as_ptr().cast(),bytes.len(),0,0)}<0{return Err(io::Error::last_os_error());}Ok(())
}
#[unsafe(no_mangle)] pub extern "C" fn klj_ashmem_initialize(fd:i32,name:*const libc::c_char)->i32{
 result(||{
  let name=if name.is_null(){Vec::new()}else{unsafe{CStr::from_ptr(name)}.to_bytes().to_vec()};
  store(fd,&original::State{name,..Default::default()})?;
  if unsafe{libc::fchflags(fd,original::MARK)}<0{return Err(io::Error::last_os_error());}Ok(())
 })
}
#[unsafe(no_mangle)] pub extern "C" fn klj_ashmem_validate(fd:i32)->i32{result(||state(fd).map(|_|()))}
#[unsafe(no_mangle)] pub extern "C" fn klj_ashmem_mapped(fd:i32,protection:u32)->i32{
 result(||{let mut value=state(fd)?;
  if protection&!7!=0{return Err(io::Error::from_raw_os_error(libc::EINVAL));}
  if protection&!value.prot!=0{return Err(io::Error::from_raw_os_error(libc::EPERM));}
  if value.mapped{return Ok(());}value.mapped=true;store(fd,&value)})
}
#[unsafe(no_mangle)] pub extern "C" fn klj_ashmem_restrict(fd:i32,protection:u32)->i32{
 result(||{let mut value=state(fd)?;if protection&!7!=0||protection&!value.prot!=0{return Err(io::Error::from_raw_os_error(libc::EINVAL));}if value.prot==protection{return Ok(());}value.prot=protection;store(fd,&value)})
}
