//! Exact C layout shared by precise original-code and native-writer owners.
use std::ffi::c_void;
#[derive(Clone,Copy)]
#[repr(C)]
pub(crate) struct State{pub(crate) x:[u64;31],pub(crate) sp:u64,pub(crate) pc:u64,pub(crate) v:[[u64;2];32],pub(crate) tp:u64,pub(crate) readonly_tp:u64,pub(crate) nzcv:u32,pub(crate) fpcr:u32,pub(crate) fpsr:u32}
#[repr(C)]
pub(crate) struct Mapping{pub(crate) address:u64,pub(crate) length:u64,pub(crate) host:*mut c_void,pub(crate) permissions:u32,pub(crate) flags:u32}
#[repr(C)]
pub(crate) struct Config{pub(crate) owner:*mut c_void,pub(crate) map:unsafe extern "C" fn(*mut c_void,u64,usize,u32,*mut Mapping)->i32,pub(crate) counter:unsafe extern "C" fn(*mut c_void)->u64,pub(crate) ctr:u32,pub(crate) frequency:u32,pub(crate) dczid:u32,pub(crate) write:unsafe extern "C" fn(*mut c_void,u64,usize)}
#[repr(C)]
pub(crate) struct Exit{pub(crate) kind:u32,pub(crate) detail:u32,pub(crate) address:u64,pub(crate) error:i32}
unsafe extern "C"{
 pub(crate) fn kl_a64_precise_create(config:Config)->*mut c_void;
 pub(crate) fn kl_a64_precise_destroy(cpu:*mut c_void);
 pub(crate) fn kl_a64_precise_set_owned_cache(cpu:*mut c_void,callback:Option<unsafe extern "C" fn(*mut c_void,u64,usize,*mut Mapping)->i32>)->i32;
 pub(crate) fn kl_a64_precise_run(cpu:*mut c_void,state:*mut State,epoch:u64)->Exit;
 pub(crate) fn kl_a64_precise_run_owned(cpu:*mut c_void,state:*mut State,epoch:u64,begin:u64,end:u64,budget:u32)->Exit;
 pub(crate) fn kl_a64_precise_clear_exclusive(cpu:*mut c_void)->i32;
 pub(crate) fn kl_a64_precise_reset(cpu:*mut c_void)->i32;
 pub(crate) fn kl_a64_precise_instruction(cpu:*const c_void)->u32;
 pub(crate) fn kl_a64_precise_instruction_address(cpu:*const c_void)->u64;
 pub(crate) fn kl_a64_precise_native_fault(signal:i32,code:i32,address:u64,mask:*const c_void)->i32;
}
const _:()={
 assert!(std::mem::size_of::<State>()==808);
 assert!(std::mem::offset_of!(State,v)==264);
 assert!(std::mem::offset_of!(State,tp)==776);
 assert!(std::mem::offset_of!(State,nzcv)==792);
 assert!(std::mem::size_of::<Mapping>()==32);
 assert!(std::mem::size_of::<Config>()==48);
 assert!(std::mem::size_of::<Exit>()==24);
};

#[cfg(test)]
#[derive(Clone,Copy)]
#[repr(C)]
pub(crate) struct Counters{pub(crate) mapped:u64,pub(crate) retired:u64,pub(crate) revalidated:u64,pub(crate) source_changed:u64}
#[cfg(test)]
unsafe extern "C"{pub(crate) fn kl_a64_precise_counters(cpu:*const c_void)->Counters;}
