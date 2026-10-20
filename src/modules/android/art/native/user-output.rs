//! Darwin kernel copyout cannot target MAP_JIT. Stage only such calls, retaining
//! the original iovec ordering and copying only bytes the native call initialized.
use std::mem::MaybeUninit;
use crate::errno::{self,Errno};

pub struct Output<'a>{original:&'a[libc::iovec],vectors:Option<Vec<libc::iovec>>,buffers:Vec<Vec<MaybeUninit<u8>>>}
fn touches_jit(address:u64,length:usize)->bool{
 if length==0{return false;}
 let Some(end)=address.checked_add(length as u64)else{return false;};let mut at=address;
 while at<end{
  let Some((start,stop,protection,_))=crate::patch::vm::region(at)else{return false;};
  if stop<=at||start>=end{return false;}
  let lo=start.max(at);let hi=stop.min(end);
  if protection&7==7&&super::arena::contains(lo,hi-lo){return true;}
  at=hi;
 }false
}
impl<'a>Output<'a>{
 pub fn new(original:&'a[libc::iovec])->Result<Self,Errno>{
  let mut output=Self{original,vectors:None,buffers:Vec::new()};
  if !original.iter().any(|part|touches_jit(part.iov_base as u64,part.iov_len)){return Ok(output);}
  original.iter().try_fold(0usize,|sum,part|sum.checked_add(part.iov_len)).filter(|n|*n<=isize::MAX as usize).ok_or(errno::EINVAL)?;
  let mut vectors=Vec::new();vectors.try_reserve_exact(original.len()).map_err(|_|errno::ENOMEM)?;
  output.buffers.try_reserve_exact(original.len()).map_err(|_|errno::ENOMEM)?;
  for part in original{
   let mut bytes=Vec::<MaybeUninit<u8>>::new();bytes.try_reserve_exact(part.iov_len).map_err(|_|errno::ENOMEM)?;
   // MaybeUninit permits reserving a large buffer without touching unwritten
   // pages. finish only reads the prefix initialized by the native operation.
   unsafe{bytes.set_len(part.iov_len);}
   vectors.push(libc::iovec{iov_base:bytes.as_mut_ptr().cast(),iov_len:part.iov_len});output.buffers.push(bytes);
  }
  output.vectors=Some(vectors);Ok(output)
 }
 pub fn vectors(&self)->&[libc::iovec]{self.vectors.as_deref().unwrap_or(self.original)}
 pub fn staged(&self)->bool{self.vectors.is_some()}
 fn finish_offset(&self,fd:i32,original:i64,result:i64)->i64{
  let copied=self.finish(result);
  if original>=0&&result>0&&copied<result&&unsafe{libc::lseek(fd,original+copied.max(0),libc::SEEK_SET)}<0{return -(errno::last()as i64);}
  copied
 }
 pub fn finish(&self,result:i64)->i64{
  if result<=0||!self.staged(){return result;}
  let mut available=result as usize;let mut prepared=0usize;let mut failure=None;
  'prepare:for part in self.original{
   let length=part.iov_len.min(available);let mut at=0usize;
   while at<length{
    let address=part.iov_base as u64+at as u64;
    let Some((start,stop,_,_))=crate::patch::vm::region(address)else{failure=Some(errno::EFAULT);break 'prepare};
    if start>address||stop<=address{failure=Some(errno::EFAULT);break 'prepare;}
    let count=(length-at).min((stop-address)as usize);
    if let Err(error)=super::user_memory::prepare_write(address,count){failure=Some(error);break 'prepare;}
    prepared+=count;at+=count;
   }
   available-=length;if available==0{break;}
  }
  // Never retain the VM epoch across blocking native I/O. Once bytes arrived,
  // freeze managed mappings only for permission rechecks and the short scatter.
  let _epoch=super::verity_pager::exclusive_mutation();let _placement=super::arena::placing();
  let mut left=prepared;let mut copied=0usize;
  for(part,bytes)in self.original.iter().zip(&self.buffers){
   let length=part.iov_len.min(left);let mut at=0usize;
   while at<length{
    let address=part.iov_base as u64+at as u64;
    let Some((start,stop,_,_))=crate::patch::vm::region(address)else{return if copied>0{copied as i64}else{-(errno::EFAULT as i64)}};
    if start>address||stop<=address{return if copied>0{copied as i64}else{-(errno::EFAULT as i64)}}
    let count=(length-at).min((stop-address)as usize);
    let initialized=unsafe{std::slice::from_raw_parts(bytes.as_ptr().cast::<u8>().add(at),count)};
    if let Err(error)=super::user_memory::write_locked(address,initialized){return if copied>0{copied as i64}else{-(error as i64)}}
    copied+=count;at+=count;
   }
   left-=length;if left==0{break;}
  }
  if let Some(error)=failure{return if copied>0{copied as i64}else{-(error as i64)}}
  // recvmsg(MSG_TRUNC) can return the full datagram length, exceeding capacity.
  // Scatter only initialized capacity while retaining that original length.
  result
 }
}

pub fn readv(fd:i32,iov:&[libc::iovec],position:Option<i64>,owned_offset:bool)->i64{
 let output=match Output::new(iov){Ok(output)=>output,Err(error)=>return -(error as i64)};
 let original=if output.staged()&&owned_offset&&position.is_none(){unsafe{libc::lseek(fd,0,libc::SEEK_CUR)}}else{-1};
 let vectors=output.vectors();
 let result=errno::check(unsafe{if let Some(position)=position{libc::preadv(fd,vectors.as_ptr(),vectors.len()as i32,position)}else{libc::readv(fd,vectors.as_ptr(),vectors.len()as i32)}}as i64);
 // The regular description retains its original offset lock across this call.
 output.finish_offset(fd,original,result)
}
pub fn read(fd:i32,address:u64,length:usize,position:Option<i64>)->i64{
 if !touches_jit(address,length){return errno::check(unsafe{if let Some(position)=position{libc::pread(fd,address as*mut _,length,position)}else{libc::read(fd,address as*mut _,length)}}as i64);}
 readv(fd,&[libc::iovec{iov_base:address as*mut _,iov_len:length}],position,false)
}

#[cfg(test)]
mod tests{
 use super::*;use std::io::Write;use std::os::fd::AsRawFd;
 #[test]
 fn output_preserves_short_prefix_eof_offsets_and_incomplete_frames(){
  if super::super::fdtab::isolated_kernel_test("sys::user_output::tests::output_preserves_short_prefix_eof_offsets_and_incomplete_frames"){return;}
  super::super::signal::install_host_handlers();
  let size=super::super::mem::PAGE as usize;
  let jit=super::super::arena::map(super::super::arena::LO,size as u64,7,libc::MAP_PRIVATE|libc::MAP_ANON|libc::MAP_JIT,-1,0).unwrap();
  let path=std::env::temp_dir().join(format!("aim-jit-output-{}",std::process::id()));
  let mut file=std::fs::OpenOptions::new().create_new(true).read(true).write(true).open(&path).unwrap();file.write_all(b"abcdef").unwrap();
  let fd=file.as_raw_fd();let mut ordinary=[0u8;3];
  let iov=[libc::iovec{iov_base:ordinary.as_mut_ptr().cast(),iov_len:3},libc::iovec{iov_base:jit as*mut _,iov_len:3},libc::iovec{iov_base:1usize as*mut _,iov_len:4096}];
  assert_eq!(readv(fd,&iov,Some(0),false),6);assert_eq!(&ordinary,b"abc");assert_eq!(super::super::user_memory::read_exact(jit,3).unwrap(),b"def");
  assert_eq!(readv(fd,&iov,Some(6),false),0);
  unsafe{libc::lseek(fd,0,libc::SEEK_SET);}
  let output=Output::new(&iov[..2]).unwrap();let v=output.vectors();
  assert_eq!(unsafe{libc::readv(fd,v.as_ptr(),v.len()as i32)},6);
  assert_eq!(super::super::mem::mprotect([jit,size as u64,libc::PROT_READ as u64,0,0,0]),0);
  assert_eq!(output.finish_offset(fd,0,6),3);assert_eq!(unsafe{libc::lseek(fd,0,libc::SEEK_CUR)},3);
  assert_eq!(super::super::mem::mprotect([jit,size as u64,7,0,0,0]),0);
  let mut pair=[0i32;2];assert_eq!(super::super::net::socketpair([1,5,0,pair.as_mut_ptr()as u64,0,0]),0);
  // A complete frame header advertises8 payload bytes, but peer EOF follows2.
  let mut frame=[0u8;18];frame[..4].copy_from_slice(&8u32.to_le_bytes());frame[16..].copy_from_slice(b"xy");
  assert_eq!(unsafe{libc::write(pair[0],frame.as_ptr().cast(),frame.len())},18);
  assert_eq!(unsafe{libc::shutdown(pair[0],libc::SHUT_WR)},0);
  super::super::user_memory::write_exact(jit,&[0x7a;8]).unwrap();
  assert_eq!(super::super::net::recvfrom([pair[1]as u64,jit,8,0x20,0,0]),2);
  assert_eq!(super::super::user_memory::read_exact(jit,8).unwrap(),b"xyzzzzzz");
  for descriptor in pair{assert_eq!(super::super::fs::close([descriptor as u64,0,0,0,0,0]),0);}
  unsafe{libc::munmap(jit as*mut _,size);}drop(file);std::fs::remove_file(path).unwrap();
 }
}

/// The same-process process_vm path uses Mach read-overwrite as its copy owner.
/// Only a rejected owned JIT destination needs an intermediate native buffer.
pub fn copy_memory(source:u64,destination:u64,length:u64)->bool{
 unsafe extern "C"{fn mach_vm_read_overwrite(task:u32,address:u64,length:u64,output:u64,actual:*mut u64)->i32;}
 let mut actual=0;
 if unsafe{mach_vm_read_overwrite(crate::patch::vm::task(),source,length,destination,&mut actual)}==0{return true;}
 if !touches_jit(destination,length as usize){return false;}
 match super::user_memory::read_exact(source,length as usize){Ok(bytes)=>super::user_memory::write_exact(destination,&bytes).is_ok(),Err(_)=>false}
}
