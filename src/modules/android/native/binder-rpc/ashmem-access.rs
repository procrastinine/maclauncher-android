//! Exact-file Seatbelt authority accompanying original anonymous ashmem FDs.
//! The descriptor owner retains each consumed lease; no parallel FD table.
use std::{ffi::{CStr,CString},io};
const CLASS:&CStr=c"com.maclauncher.android.ashmem";
const ATTRIBUTE:&CStr=c"com.maclauncher.android.ashmem-access";
unsafe extern "C" {
 fn sandbox_extension_issue_file(class:*const libc::c_char,path:*const libc::c_char,flags:u32)->*mut libc::c_char;
 fn sandbox_extension_consume(token:*const libc::c_char)->i64;
 fn sandbox_extension_release(handle:i64)->libc::c_int;
}
fn error()->io::Error{io::Error::last_os_error()}
fn marked(fd:i32)->io::Result<()>{
 let mut stat:libc::stat=unsafe{std::mem::zeroed()};
 if unsafe{libc::fstat(fd,&mut stat)}<0{return Err(error());}
 if stat.st_mode&libc::S_IFMT!=libc::S_IFREG||stat.st_nlink!=0||stat.st_flags&super::MARK!=super::MARK{return Err(io::Error::from_raw_os_error(libc::ENOTTY));}Ok(())
}
pub fn issue(fd:i32,path:&CStr)->io::Result<()>{
 let pointer=unsafe{sandbox_extension_issue_file(CLASS.as_ptr(),path.as_ptr(),0)};
 if pointer.is_null(){return Err(error());}
 let token=unsafe{CStr::from_ptr(pointer)}.to_bytes();
 let result=unsafe{libc::fsetxattr(fd,ATTRIBUTE.as_ptr(),token.as_ptr().cast(),token.len(),0,0)};
 let failure=if result<0{Some(error())}else{None};unsafe{libc::free(pointer.cast())};
 failure.map_or(Ok(()),Err)
}
pub fn token(fd:i32)->io::Result<Vec<u8>>{
 let mut stat:libc::stat=unsafe{std::mem::zeroed()};
 if unsafe{libc::fstat(fd,&mut stat)}<0{return Err(error());}
 if stat.st_mode&libc::S_IFMT!=libc::S_IFREG||stat.st_nlink!=0||stat.st_flags&super::MARK!=super::MARK{return Ok(Vec::new());}
 let size=unsafe{libc::fgetxattr(fd,ATTRIBUTE.as_ptr(),std::ptr::null_mut(),0,0,0)};
 if size<0{let failure=error();return if failure.raw_os_error()==Some(libc::ENOATTR){Ok(Vec::new())}else{Err(failure)};}
 let mut bytes=vec![0;size as usize];let count=unsafe{libc::fgetxattr(fd,ATTRIBUTE.as_ptr(),bytes.as_mut_ptr().cast(),bytes.len(),0,0)};
 if count<0{return Err(error());}bytes.truncate(count as usize);Ok(bytes)
}
pub struct Lease(i64);
impl Lease{pub fn into_raw(self)->i64{let handle=self.0;std::mem::forget(self);handle}}
impl Drop for Lease{fn drop(&mut self){unsafe{sandbox_extension_release(self.0)};}}
pub fn consume(fd:i32,bytes:&[u8])->io::Result<Lease>{
 marked(fd)?;let string=CString::new(bytes).map_err(|_|io::Error::from_raw_os_error(libc::EINVAL))?;
 let handle=unsafe{sandbox_extension_consume(string.as_ptr())};if handle<0{return Err(error());}
 let lease=Lease(handle);
 if token(fd)?!=bytes{return Err(io::Error::from_raw_os_error(libc::EINVAL));}Ok(lease)
}
fn code<T>(value:io::Result<T>,success:impl FnOnce(T)->i64)->i64{match value{Ok(value)=>success(value),Err(error)=>-i64::from(error.raw_os_error().unwrap_or(libc::EIO))}}
#[unsafe(no_mangle)]pub unsafe extern "C" fn klj_ashmem_capability_issue(fd:i32,path:*const libc::c_char)->i64{code(issue(fd,unsafe{CStr::from_ptr(path)}),|_|0)}
#[unsafe(no_mangle)]pub unsafe extern "C" fn klj_ashmem_capability_export(fd:i32,buffer:*mut u8,capacity:usize)->i64{
 code(token(fd),|bytes|{if buffer.is_null(){return bytes.len()as i64;}if bytes.len()>capacity{return -i64::from(libc::ERANGE);}unsafe{std::ptr::copy_nonoverlapping(bytes.as_ptr(),buffer,bytes.len())};bytes.len()as i64})
}
#[unsafe(no_mangle)]pub unsafe extern "C" fn klj_ashmem_capability_consume(fd:i32,buffer:*const u8,size:usize)->i64{code(consume(fd,unsafe{std::slice::from_raw_parts(buffer,size)}),Lease::into_raw)}
#[unsafe(no_mangle)]pub extern "C" fn klj_ashmem_capability_release(handle:i64){if handle>=0{drop(Lease(handle));}}
