//! Optional host-side ownership of typed guest argument blocks. The guest ABI,
//! field values and public argument-validation rules remain unchanged.
use core::sync::atomic::{AtomicPtr,Ordering};
pub type Resolver=unsafe fn(u64,u64,usize)->Result<u64,i64>;
static RESOLVER:AtomicPtr<()>=AtomicPtr::new(core::ptr::null_mut());
pub fn install(resolver:Resolver)->Result<(),()> {
    let pointer=resolver as *mut ();
    match RESOLVER.compare_exchange(core::ptr::null_mut(),pointer,Ordering::AcqRel,Ordering::Acquire){
        Ok(_)=>Ok(()),Err(current)if current==pointer=>Ok(()),Err(_)=>Err(())
    }
}
pub unsafe fn argument(address:u64,length:u64,alignment:usize)->Result<u64,i64>{
    let pointer=RESOLVER.load(Ordering::Acquire);
    if pointer.is_null(){return Ok(address);}
    let resolve:Resolver=unsafe{core::mem::transmute(pointer)};
    unsafe{resolve(address,length,alignment)}
}
