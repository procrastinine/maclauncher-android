//! Fault-contained writes to owned guest MAP_JIT memory. Mach VM copies reject
//! these mappings even while a CPU can write them. No guest protection changes.
use std::arch::global_asm;
use std::sync::OnceLock;
use crate::errno::{self,Errno};
use super::sigframe::DarwinMcontext;

// No stack frame or Rust references span the faulting instructions. The existing
// signal owner redirects only this exact instruction range to its error return.
global_asm!(r#"
.text
.p2align 2
.globl _maclauncher_jit_copy
.private_extern _maclauncher_jit_copy
_maclauncher_jit_copy:
    cbz x2, 2f
.globl _maclauncher_jit_copy_begin
.private_extern _maclauncher_jit_copy_begin
_maclauncher_jit_copy_begin:
1:  ldrb w3, [x1], #1
    strb w3, [x0], #1
    subs x2, x2, #1
    b.ne 1b
.globl _maclauncher_jit_copy_end
.private_extern _maclauncher_jit_copy_end
_maclauncher_jit_copy_end:
2:  mov w0, #0
    ret
.globl _maclauncher_jit_copy_fault
.private_extern _maclauncher_jit_copy_fault
_maclauncher_jit_copy_fault:
    mov w0, #14
    ret
"#);
unsafe extern "C" {
 fn maclauncher_jit_copy(destination:u64,source:*const u8,length:usize)->i32;
 fn maclauncher_jit_copy_begin();fn maclauncher_jit_copy_end();fn maclauncher_jit_copy_fault();
}

pub fn recover(state:&mut DarwinMcontext)->bool{
 let begin=maclauncher_jit_copy_begin as *const () as u64;
 let end=maclauncher_jit_copy_end as *const () as u64;
 if state.pc<begin||state.pc>=end{return false;}
 state.pc=maclauncher_jit_copy_fault as *const () as u64;true
}

// Public pthread APIs have no current-view getter. A store to one immutable,
// owned probe page observes the view without changing it. The precise copy
// recovery runs before the normal JIT fault owner, so a denied store stays denied.
// Threads all store the same byte; no Rust reference aliases this storage.
fn protected()->Result<bool,Errno>{
 static PROBE:OnceLock<Option<usize>>=OnceLock::new();
 let page=PROBE.get_or_init(||{
  let value=unsafe{libc::mmap(std::ptr::null_mut(),super::mem::PAGE as usize,
   libc::PROT_READ|libc::PROT_WRITE|libc::PROT_EXEC,
   libc::MAP_PRIVATE|libc::MAP_ANON|libc::MAP_JIT,-1,0)};
  (value!=libc::MAP_FAILED).then_some(value as usize)
 }).ok_or(errno::EFAULT)?;
 let zero=0u8;
 Ok(unsafe{maclauncher_jit_copy(page as u64,&zero,1)}!=0)
}
struct View{restore:bool}
impl View{
 fn writable()->Result<Self,Errno>{let restore=protected()?;if restore{unsafe{libc::pthread_jit_write_protect_np(0)}}Ok(Self{restore})}
}
impl Drop for View{fn drop(&mut self){if self.restore{unsafe{libc::pthread_jit_write_protect_np(1)}}}}

pub fn write(address:u64,bytes:&[u8])->Result<(),Errno>{
 let _epoch=super::verity_pager::mutation();let _placement=super::arena::placing();
 write_locked(address,bytes)
}
/// Caller retains the existing VM epoch and placement guards throughout copy.
pub(crate) fn write_locked(address:u64,bytes:&[u8])->Result<(),Errno>{
 if bytes.is_empty(){return Ok(());}
 if !super::arena::contains(address,bytes.len()as u64){return Err(errno::EFAULT);}
 // Share the existing mapping epoch: protection/proof replacement is excluded.
 // Concurrent unmap/replace is still fault-contained by the exact assembly range.
 let end=address.checked_add(bytes.len()as u64).ok_or(errno::EFAULT)?;
 let mut at=address;
 const RWX:i32=libc::PROT_READ|libc::PROT_WRITE|libc::PROT_EXEC;
 while at<end{
  let (start,stop,protection,_)=crate::patch::vm::region(at).ok_or(errno::EFAULT)?;
  if start>at||stop<=at||protection&RWX!=RWX{return Err(errno::EFAULT);}
  at=stop.min(end);
 }
 let _view=View::writable()?;
 if unsafe{maclauncher_jit_copy(address,bytes.as_ptr(),bytes.len())}!=0{return Err(errno::EFAULT);}
 Ok(())
}

#[cfg(test)]
mod tests{
 use super::*;
 #[test]
 fn jit_destinations_preserve_view_and_contain_invalid_buffers(){
  super::super::signal::install_host_handlers();
  let size=super::super::mem::PAGE as usize;
  let address=super::super::arena::map(super::super::arena::LO,size as u64,
   libc::PROT_READ|libc::PROT_WRITE|libc::PROT_EXEC,
   libc::MAP_PRIVATE|libc::MAP_ANON|libc::MAP_JIT,-1,0).unwrap();
  let input=[0x6bu8;128];
  for view in [1,0]{
   unsafe{libc::pthread_jit_write_protect_np(view);}
   assert_eq!(protected(),Ok(view==1));
   write(address+7,&input).unwrap();
   assert_eq!(protected(),Ok(view==1));
   assert_eq!(super::super::user_memory::read_exact(address+7,input.len()).unwrap(),input);
   assert_eq!(write(1,&input),Err(errno::EFAULT));
   assert_eq!(protected(),Ok(view==1));
   // Models a mapping disappearing after validation: only the assembly memory
   // instructions can fault, and their error return preserves the thread view.
   assert_eq!(unsafe{maclauncher_jit_copy(1,input.as_ptr(),input.len())},14);
   assert_eq!(protected(),Ok(view==1));
  }
  unsafe{libc::pthread_jit_write_protect_np(1);libc::munmap(address as*mut _,size);}
  assert_eq!(write(address,&input),Err(errno::EFAULT));
  assert_eq!(protected(),Ok(true));
  let mut ordinary=[0u8;128];
  super::super::user_memory::write_exact(ordinary.as_mut_ptr()as u64,&input).unwrap();
  assert_eq!(ordinary,input);
  let rwx=unsafe{libc::mmap(std::ptr::null_mut(),size,7,libc::MAP_PRIVATE|libc::MAP_ANON,-1,0)};
  if rwx!=libc::MAP_FAILED{
   assert!(!super::super::arena::contains(rwx as u64,size as u64));
   super::super::user_memory::write_exact(rwx as u64,&input).unwrap();
   assert_eq!(super::super::user_memory::read_exact(rwx as u64,input.len()).unwrap(),input);
   unsafe{libc::munmap(rwx,size);}
  }else{assert_eq!(std::io::Error::last_os_error().raw_os_error(),Some(libc::EACCES));println!("ordinary non-JIT RWX allocation rejected by Darwin; ordinary RW Mach path verified");}
  // Race native invalidation against a real guarded store. Managed protection
  // replacement uses the original epoch and therefore waits for the copy.
  for view in [1,0]{for protection in [false,true]{
   let length=size*256;
   let address=super::super::arena::map(super::super::arena::LO,length as u64,7,libc::MAP_PRIVATE|libc::MAP_ANON|libc::MAP_JIT,-1,0).unwrap();
   let input=vec![0x3cu8;length];let barrier=std::sync::Arc::new(std::sync::Barrier::new(2));let worker_gate=barrier.clone();
   let worker=std::thread::spawn(move||{
    worker_gate.wait();std::thread::sleep(std::time::Duration::from_micros(50));
    if protection{assert_eq!(super::super::mem::mprotect([address,length as u64,libc::PROT_READ as u64,0,0,0]),0);}
    else{assert_eq!(unsafe{libc::munmap(address as*mut _,length)},0);}
   });
   unsafe{libc::pthread_jit_write_protect_np(view);}barrier.wait();
   let result=write(address,&input);assert!(matches!(result,Ok(())|Err(errno::EFAULT)));
   worker.join().unwrap();assert_eq!(protected(),Ok(view==1));
   if protection{unsafe{libc::munmap(address as*mut _,length);}}
  }}
  unsafe{libc::pthread_jit_write_protect_np(1);}
 }
}
