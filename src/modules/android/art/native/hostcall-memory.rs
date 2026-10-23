//! Typed host-call arguments retain their actual backing until dispatch returns.
//! Only the outer argument block is resolved. Nested buffers, handles, offsets
//! and callback addresses retain the original ABI and their own owners.
use std::{cell::RefCell,sync::Once};
use crate::patch::vm;
use super::mutable_memory::{self,Lease};
const PAGE:u64=16384;
const R:i32=libc::PROT_READ;
const RW:i32=R|libc::PROT_WRITE;
const EFAULT:i64=-(crate::errno::EFAULT as i64);
struct Alias{address:u64,length:u64}
impl Drop for Alias{fn drop(&mut self){unsafe{vm::mach_vm_deallocate(vm::task(),self.address,self.length);}}}
#[allow(dead_code)]
enum Retained{Mapping(Lease),Alias(Alias)}
#[derive(Default)]
struct Owner{depth:usize,retained:Vec<Retained>}
thread_local!{static OWNER:RefCell<Owner>=const{RefCell::new(Owner{depth:0,retained:Vec::new()})};}
static INSTALL:Once=Once::new();
pub(crate) struct Scope{checkpoint:usize}
impl Scope{
    pub(crate) fn enter()->Self{
        INSTALL.call_once(||aim_hostcall::maclauncher_memory::install(resolve).expect("Guest argument resolver already installed"));
        let checkpoint=OWNER.with(|owner|{let mut owner=owner.borrow_mut();owner.depth+=1;owner.retained.len()});
        Self{checkpoint}
    }
}
impl Drop for Scope{
    fn drop(&mut self){OWNER.with(|owner|{let mut owner=owner.borrow_mut();owner.retained.truncate(self.checkpoint);owner.depth-=1;});}
}
fn retain(value:Retained)->Result<(),i64>{
    OWNER.with(|owner|{let mut owner=owner.borrow_mut();if owner.depth==0{return Err(EFAULT);}owner.retained.try_reserve(1).map_err(|_|-(crate::errno::ENOMEM as i64))?;owner.retained.push(value);Ok(())})
}
fn permitted(address:u64,end:u64,access:i32)->Result<(),i64>{
    let ranges=mutable_memory::ranges(address,end-address);let mut at=address;
    while at<end{
        let (start,stop,actual,_)=vm::region(at).ok_or(EFAULT)?;
        if start>at||stop<=at{return Err(EFAULT);}
        let owned=ranges.iter().find(|(start,stop,_)|*start<=at&&at<*stop);
        let (protection,next)=if let Some((_,stop,protection))=owned{(*protection,*stop)}else{
            (actual,ranges.iter().find(|(start,_,_)|*start>at).map_or(end,|(start,_,_)|*start))
        };
        if protection&access!=access{return Err(EFAULT);}
        at=stop.min(next).min(end);
    }
    Ok(())
}
fn shared_alias(address:u64,length:u64,alignment:usize,access:i32)->Result<Alias,i64>{
    let mut target=0;let(mut current,mut maximum)=(0,0);
    let mask=PAGE.max(alignment as u64)-1;
    let result=unsafe{vm::mach_vm_remap(vm::task(),&mut target,length,mask,vm::VM_FLAGS_ANYWHERE,vm::task(),address,0,&mut current,&mut maximum,vm::VM_INHERIT_COPY)};
    if result!=0{return Err(-(crate::errno::ENOMEM as i64));}
    let alias=Alias{address:target,length};
    if unsafe{vm::mach_vm_protect(vm::task(),target,length,0,access)}!=0{return Err(EFAULT);}
    Ok(alias)
}
unsafe fn resolve(address:u64,length:u64,alignment:usize)->Result<u64,i64>{
    unsafe{resolve_access(address,length,alignment,RW)}
}
unsafe fn resolve_access(address:u64,length:u64,alignment:usize,access:i32)->Result<u64,i64>{
    if length==0||!mutable_memory::has_mappings()||!mutable_memory::overlaps(address,length){return Ok(address);}
    let end=address.checked_add(length).ok_or(EFAULT)?;
    let length_usize=usize::try_from(length).map_err(|_|EFAULT)?;
    if !alignment.is_power_of_two()||!OWNER.with(|owner|owner.borrow().depth!=0){return Err(EFAULT);}
    // Populate lazy input/output pages before the VM read epoch. No pager,
    // mapping or transaction lock survives the argument-resolution call.
    super::user_memory::prepare_read(address,length_usize).map_err(|error|-(error as i64))?;
    if access&libc::PROT_WRITE!=0{super::user_memory::prepare_write(address,length_usize).map_err(|error|-(error as i64))?;}
    let _epoch=super::verity_pager::mutation();
    if let Some(lease)=mutable_memory::mapping(address,length,access){
        let pointer=lease.address+address-lease.start;
        if pointer%(alignment as u64)==0{retain(Retained::Mapping(lease))?;return Ok(pointer);}
    }
    permitted(address,end,access)?;
    let start=address&!(PAGE-1);let stop=end.checked_add(PAGE-1).ok_or(EFAULT)?&!(PAGE-1);
    let alias=shared_alias(start,stop-start,alignment,access)?;
    let pointer=alias.address+address-start;
    if pointer%(alignment as u64)!=0{return Err(EFAULT);}
    retain(Retained::Alias(alias))?;Ok(pointer)
}

type PacketResolver=unsafe extern "C" fn(u64,u64,u64,u32,*mut u64)->i64;
type InstallPackets=unsafe extern "C" fn(u32,PacketResolver)->i64;
/// Older host libraries retain their original immutable-packet path. A supplied
/// setter must accept this exact version before any extension call is visible.
pub(crate) unsafe fn install_extension(handle:*mut libc::c_void)->Result<(),String>{
    let pointer=unsafe{libc::dlsym(handle,c"maclauncher_art_install_packet_resolver".as_ptr())};
    if pointer.is_null(){return Ok(());}
    let install:InstallPackets=unsafe{std::mem::transmute(pointer)};
    let error=unsafe{install(1,resolve_packet)};
    if error!=0{return Err(format!("Host packet resolver installation failed: errno {}",error.saturating_neg()));}
    Ok(())
}
unsafe extern "C" fn resolve_packet(address:u64,length:u64,alignment:u64,access:u32,output:*mut u64)->i64{
    let saved=unsafe{*libc::__error()};
    let result=std::panic::catch_unwind(||{
        if output.is_null()||!matches!(access,1|3){return Err(-(crate::errno::EINVAL as i64));}
        let alignment=usize::try_from(alignment).map_err(|_|-(crate::errno::EINVAL as i64))?;
        let pointer=unsafe{resolve_access(address,length,alignment,access as i32)}?;
        unsafe{output.write(pointer)};Ok(())
    });
    unsafe{*libc::__error()=saved;}
    match result{Ok(Ok(()))=>0,Ok(Err(error))=>error,Err(_)=>EFAULT}
}

#[cfg(test)]
mod tests{
    use super::*;
    use aim_hostcall::args_mut;
    #[repr(C)]struct Packet{input:u64,output:u64}
    fn packet(address:u64,input:u64){unsafe{(address as*mut Packet).write(Packet{input,output:0});}}
    fn dispatch(address:u64)->i64{
        let _scope=Scope::enter();
        let value=unsafe{args_mut::<Packet>(address,16)}.unwrap();value.output=value.input+7;
        // A nested guest callback observes the write before this host call
        // returns; a staged copyback would fail this assertion.
        assert_eq!(unsafe{((address+8)as*const u64).read()},value.output);
        0
    }
    #[test]
    fn typed_argument_blocks_keep_live_shared_backing_and_original_validation(){
        let _transaction=mutable_memory::transaction();mutable_memory::reset();
        let raw=unsafe{libc::mmap(std::ptr::null_mut(),(2*PAGE)as usize,RW,libc::MAP_PRIVATE|libc::MAP_ANON,-1,0)};
        assert_ne!(raw,libc::MAP_FAILED);let base=raw as u64;
        let original=vec![0;PAGE as usize];
        mutable_memory::prepare(base,&original,RW|libc::PROT_EXEC).unwrap().install(|publish|publish()).unwrap();
        let lease=mutable_memory::mapping(base,PAGE,RW).unwrap();
        packet(lease.address+32,41);assert_eq!(dispatch(base+32),0);
        assert_eq!(unsafe{((base+40)as*const u64).read()},48);assert_eq!(vm::region(base).unwrap().2,libc::PROT_READ);
        // The next packet crosses the owned/unowned boundary. Its temporary
        // Mach view shares both original pages without changing public access.
        unsafe{((lease.address+PAGE-8)as*mut u64).write(0);}
        unsafe{((base+PAGE)as*mut u64).write(0);}
        assert_eq!(dispatch(base+PAGE-8),0);assert_eq!(unsafe{((base+PAGE)as*const u64).read()},7);
        assert_eq!(vm::region(base).unwrap().2,libc::PROT_READ);assert_eq!(vm::region(base+PAGE).unwrap().2,RW);
        assert_eq!(crate::hostcall::call(0,0,0,0),aim_hostcall::ABI_VERSION as i64);
        assert_eq!(crate::hostcall::call(u64::MAX,1,base,16),-(aim_hostcall::errno::ENOSYS as i64));
        // Reentrant calls retain outer leases, and a replacement public mapping
        // cannot recycle the backing still held by a host call.
        let outer=Scope::enter();let value=unsafe{args_mut::<Packet>(base+32,16)}.unwrap();
        {let _nested=Scope::enter();let child=unsafe{args_mut::<Packet>(base+64,16)}.unwrap();child.output=99;}
        value.output=123;assert_eq!(unsafe{((base+40)as*const u64).read()},123);
        mutable_memory::prepare(base,&vec![0x55;PAGE as usize],RW|libc::PROT_EXEC).unwrap().install(|publish|publish()).unwrap();
        value.output=456;assert_eq!(unsafe{((base+40)as*const u64).read()},0x5555555555555555);
        assert_eq!(value.output,456);drop(outer);
        assert_eq!(unsafe{args_mut::<Packet>(base+1,16)}.err(),Some(-(aim_hostcall::errno::EINVAL as i64)));
        assert_eq!(unsafe{args_mut::<Packet>(base,8)}.err(),Some(-(aim_hostcall::errno::EINVAL as i64)));
        assert_eq!(unsafe{args_mut::<Packet>(base,16)}.err(),Some(EFAULT));
        {let _scope=Scope::enter();mutable_memory::protect(base,PAGE,libc::PROT_READ|libc::PROT_EXEC).unwrap().unwrap();assert_eq!(unsafe{args_mut::<Packet>(base,16)}.err(),Some(EFAULT));}
        mutable_memory::forget(base,PAGE);unsafe{libc::munmap(raw,(2*PAGE)as usize);}drop(lease);
    }
    #[test]
    #[ignore="requires the matching native host extension"]
    fn foreign_c_packets_retain_shared_clock_output_and_read_only_log_input(){
        let library=std::env::var_os("MACLAUNCHER_ART_HOST_PACKET_LIBRARY").expect("native host extension required");
        crate::hostcall::load_extension(std::path::Path::new(&library)).unwrap();
        let _transaction=mutable_memory::transaction();mutable_memory::reset();
        let raw=unsafe{libc::mmap(std::ptr::null_mut(),(2*PAGE)as usize,RW,libc::MAP_PRIVATE|libc::MAP_ANON,-1,0)};
        assert_ne!(raw,libc::MAP_FAILED);let base=raw as u64;
        mutable_memory::prepare(base,&vec![0;PAGE as usize],RW|libc::PROT_EXEC).unwrap().install(|publish|publish()).unwrap();
        let lease=mutable_memory::mapping(base,PAGE,RW).unwrap();
        assert_eq!(crate::hostcall::call(0x4d4c,1,base+32,8),0);
        let first=unsafe{((base+32)as*const u64).read()};assert!(first>0);
        assert_eq!(crate::hostcall::call(0x4d4c,1,base+32,7),-22);
        assert_eq!(crate::hostcall::call(0x4d4c,1,base+33,8),-22);
        assert_eq!(crate::hostcall::call(0x4d4c,1,0,8),-22);
        assert_eq!(unsafe{((base+32)as*const u64).read()},first);
        assert_eq!(vm::region(base).unwrap().2,R);
        #[repr(C)]struct Log{priority:i32,reserved:u32,tag:u64,message:u64}
        let log=Log{priority:4,reserved:0,tag:c"typed-packet".as_ptr()as u64,message:c"read-only original input".as_ptr()as u64};
        unsafe{((lease.address+64)as*mut Log).write(log);}
        // This input packet crosses into an ordinary writable VM page. Its
        // temporary alias must require/read only logical READ, never RW.
        unsafe{((lease.address+PAGE-8)as*mut u64).write(4);((base+PAGE)as*mut u64).write(c"mixed-packet".as_ptr()as u64);((base+PAGE+8)as*mut u64).write(c"read-only shared alias".as_ptr()as u64);}
        mutable_memory::protect(base,PAGE,R|libc::PROT_EXEC).unwrap().unwrap();
        assert!(crate::hostcall::call(0x4d4c,14,base+64,24)>0);
        assert!(crate::hostcall::call(0x4d4c,14,base+PAGE-8,24)>0);
        assert_eq!(crate::hostcall::call(0x4d4c,1,base+32,8),EFAULT);
        assert_eq!(vm::region(base).unwrap().2,R);assert_eq!(vm::region(base+PAGE).unwrap().2,RW);
        {let _scope=Scope::enter();let mut output=0x55;
            unsafe{*libc::__error()=libc::EDOM;}
            assert_eq!(unsafe{resolve_packet(base+64,24,8,2,&mut output)},-22);
            assert_eq!(output,0x55);assert_eq!(unsafe{*libc::__error()},libc::EDOM);
        }
        mutable_memory::forget(base,PAGE);unsafe{libc::munmap(raw,(2*PAGE)as usize);}drop(lease);
    }

}
