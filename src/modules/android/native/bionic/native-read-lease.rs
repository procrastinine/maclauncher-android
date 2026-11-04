//! A synchronous native consumer lease over an original guest open description.
//! Tokens are opaque and local to the physical calling thread. No token is
//! dereferenced, and native consumers never receive a guest descriptor number.
use super::{Kind,Pinned};
use crate::{context,errno::{self,Errno}};
use std::{cell::RefCell,collections::HashMap,ffi::c_void,os::fd::AsRawFd,sync::atomic::{AtomicUsize,Ordering}};

struct NativeErrno(i32);
impl NativeErrno { fn save()->Self { Self(unsafe{*libc::__error()}) } }
impl Drop for NativeErrno { fn drop(&mut self) { unsafe{*libc::__error()=self.0;} } }

#[derive(Clone,Copy,Debug,PartialEq,Eq)]
struct Owner { process:i32,managed_process:i32,thread:i32,context:usize,tp:u64 }
// Both original Bionic pthreads and admitted Java pthreads enter through the
// original hostcall context. Confirm the registered physical thread as well as
// its canonical Android lease; a foreign-admission ticket is not required.
fn physical_thread(tid:i32)->bool{
 let mut physical=0u64;
 let status=unsafe{libc::pthread_threadid_np(0,&mut physical)};
 status==0 && physical!=0 && super::super::thread::host_thread_id(tid)==Some(physical)
}
impl Owner {
    fn current()->Result<Self,Errno> {
        let raw=context::current_ctx();
        if raw.is_null(){return Err(errno::EPERM);}
        let ctx=unsafe{&*raw};
        if ctx.in_host==0||ctx.callback_sp==0||context::guest_tp()==0{return Err(errno::EPERM);}
        let thread=super::super::thread::current().ok_or(errno::EPERM)?;
        let managed_process=match crate::process_identity::process(){Some(Ok(id))=>id,_=>return Err(errno::EPERM)};
        if ctx.tid!=thread.tid as u64||crate::process_identity::thread()!=Some(Ok(thread.tid))||!physical_thread(thread.tid){return Err(errno::EPERM);}
        Ok(Self{process:unsafe{libc::getpid()},managed_process,thread:thread.tid,context:raw as usize,tp:context::guest_tp()})
    }
}
struct Lease { owner:Owner,_pin:Pinned }
thread_local! { static LEASES:RefCell<HashMap<usize,Lease>>=RefCell::new(HashMap::new()); }
static NEXT:AtomicUsize=AtomicUsize::new(1);

/// `pin_guest` performs publication, alias adoption and kernel duplication under
/// the original lifecycle owner. All its descriptor-table locks are released
/// before this function returns or a native publisher is entered.
fn readable(fd:i32)->Result<Pinned,Errno> { validate(super::pin_guest(fd)?) }
fn validate(pin:Pinned)->Result<Pinned,Errno> {
    let fd=pin.descriptor().as_raw_fd();
    if pin.fuse.is_some()||super::super::fuse_device::is_typed(fd){return Err(errno::EOPNOTSUPP);}
    match pin.kind() {
        Some(Kind::Path(_))=>return Err(errno::EBADF),
        Some(Kind::Regular(description))=>{
            if description.flags&super::super::fs::O_PATH!=0||!matches!(description.flags&3,0|2){return Err(errno::EBADF);}
            // This owner verifies every read in user space. A decoder retains
            // its own duplicate after this call, so a temporary metadata check
            // cannot preserve that ongoing contract.
            return Err(errno::EOPNOTSUPP);
        }
        None=>{},
        Some(_)=>return Err(errno::EOPNOTSUPP),
    }
    let mut stat:libc::stat=unsafe{std::mem::zeroed()};
    if unsafe{libc::fstat(fd,&mut stat)}<0{return Err(errno::last());}
    if stat.st_mode&libc::S_IFMT!=libc::S_IFREG{return Err(errno::EOPNOTSUPP);}
    let flags=unsafe{libc::fcntl(fd,libc::F_GETFL)};
    if flags<0{return Err(errno::last());}
    if flags&libc::O_EVTONLY!=0||!matches!(flags&libc::O_ACCMODE,libc::O_RDONLY|libc::O_RDWR){return Err(errno::EBADF);}
    Ok(pin)
}
fn retain(owner:Owner,pin:Pinned)->Result<(i32,usize),Errno> {
    let token=NEXT.fetch_update(Ordering::Relaxed,Ordering::Relaxed,|n|n.checked_add(1)).map_err(|_|75 /* Linux EOVERFLOW */)?;
    let fd=pin.descriptor().as_raw_fd();
    LEASES.with(|leases|{assert!(leases.borrow_mut().insert(token,Lease{owner,_pin:pin}).is_none());});
    Ok((fd,token))
}
fn release_owned(owner:Owner,token:usize)->Result<(),Errno> {
    let lease=LEASES.with(|leases|{
        let mut leases=leases.borrow_mut();
        let lease=leases.get(&token).ok_or(errno::EBADF)?;
        if lease.owner!=owner{return Err(errno::EPERM);}
        Ok(leases.remove(&token).unwrap())
    })?;
    // Pinned retirement can enter the original fd lifecycle. No registry borrow
    // or descriptor-table lock is held while the original owner is dropped.
    drop(lease);Ok(())
}

#[repr(C)]
pub struct Api {
    size:u32,version:u32,
    borrow:unsafe extern "C" fn(i32,*mut i32,*mut *mut c_void)->i64,
    release:extern "C" fn(*mut c_void)->i64,
}
const _:()={assert!(std::mem::size_of::<Api>()==24);assert!(std::mem::offset_of!(Api,borrow)==8);assert!(std::mem::offset_of!(Api,release)==16);};
unsafe extern "C" fn borrow(fd:i32,native:*mut i32,opaque:*mut *mut c_void)->i64 {
    let _errno=NativeErrno::save();
    if native.is_null()||opaque.is_null(){return -(errno::EINVAL as i64);}
    unsafe{native.write(-1);opaque.write(std::ptr::null_mut());}
    let outcome=(||{let owner=Owner::current()?;retain(owner,readable(fd)?)})();
    match outcome { Ok((fd,token))=>{unsafe{native.write(fd);opaque.write(token as *mut c_void);}0},Err(error)=>-(error as i64) }
}
extern "C" fn release(opaque:*mut c_void)->i64 {
    let _errno=NativeErrno::save();
    match Owner::current().and_then(|owner|release_owned(owner,opaque as usize)){Ok(())=>0,Err(error)=>-(error as i64)}
}
static API:Api=Api{size:std::mem::size_of::<Api>()as u32,version:1,borrow,release};
#[unsafe(no_mangle)]
pub extern "C" fn klj_bionic_read_fd_api_v1()->*const Api { &API }

#[cfg(test)]
mod tests {
    use super::*;
    use std::{fs::File,os::fd::{AsFd,FromRawFd},os::unix::fs::OpenOptionsExt};
    fn owner()->Owner { Owner{process:unsafe{libc::getpid()},managed_process:1,thread:1,context:1,tp:1} }
    fn published(file:&File) { super::super::publish_guest(file.as_raw_fd()).unwrap(); }
    fn close(file:File) { let fd=file.as_raw_fd();super::super::withdraw_guest(fd).unwrap();super::super::on_close(fd);drop(file); }
    #[test]
    fn original_pin_read_lease_visibility_access_and_retirement() {
        const NAME:&str="sys::fdtab::native_read_lease::tests::original_pin_read_lease_visibility_access_and_retirement";
        if super::super::isolated_kernel_test(NAME){return;}
        let (_guard,root)=crate::vfs::test_view();
        let path=root.join("data/native-read-lease");std::fs::write(&path,b"original native bytes").unwrap();
        let mut native=19;let mut token=19usize as *mut c_void;
        unsafe{*libc::__error()=libc::EACCES;}
        assert_eq!(unsafe{borrow(-1,&mut native,&mut token)},-(errno::EPERM as i64));
        assert_eq!((native,token),(-1,std::ptr::null_mut()));assert_eq!(unsafe{*libc::__error()},libc::EACCES);
        assert_eq!(release(19usize as *mut c_void),-(errno::EPERM as i64));assert_eq!(unsafe{*libc::__error()},libc::EACCES);
        assert!(matches!(readable(-1),Err(errno::EBADF)));
        let hidden=File::open(&path).unwrap();assert!(matches!(readable(hidden.as_raw_fd()),Err(errno::EBADF)));drop(hidden);
        let file=File::open(&path).unwrap();published(&file);
        let pin=readable(file.as_raw_fd()).unwrap();let (native,token)=retain(owner(),pin).unwrap();
        assert_ne!(native,file.as_raw_fd());assert!(super::super::is_hidden(native));assert!(super::super::TABLE.try_write().is_ok());
        assert!(matches!(super::super::require_guest_visible(native),Err(errno::EBADF)));
        close(file); // Guest close cannot revoke the already captured object.
        let mut bytes=[0u8;21];assert_eq!(unsafe{libc::pread(native,bytes.as_mut_ptr().cast(),bytes.len(),0)},21);assert_eq!(&bytes,b"original native bytes");
        let copy=unsafe{libc::fcntl(native,libc::F_DUPFD_CLOEXEC,0)};assert!(copy>=0);let duplicate=unsafe{File::from_raw_fd(copy)};
        let mut foreign=owner();foreign.context+=1;
        assert!(matches!(release_owned(foreign,token),Err(errno::EPERM)));assert!(unsafe{libc::fcntl(native,libc::F_GETFD)}>=0);
        assert_eq!(std::thread::spawn(move||release_owned(owner(),token)).join().unwrap(),Err(errno::EBADF));
        release_owned(owner(),token).unwrap();assert!(unsafe{libc::fcntl(native,libc::F_GETFD)}<0);
        assert_eq!(release_owned(owner(),token),Err(errno::EBADF));
        assert_eq!(unsafe{libc::pread(duplicate.as_raw_fd(),bytes.as_mut_ptr().cast(),bytes.len(),0)},21);drop(duplicate);
        for write in [false,true] {
            let file=File::options().read(!write).write(write).open(&path).unwrap();published(&file);
            if write {assert!(matches!(readable(file.as_raw_fd()),Err(errno::EBADF)));}
            else {drop(readable(file.as_raw_fd()).unwrap());}
            close(file);
        }
        let file=File::options().read(true).write(true).open(&path).unwrap();published(&file);drop(readable(file.as_raw_fd()).unwrap());close(file);
        let file=File::options().read(true).custom_flags(libc::O_EVTONLY).open(&path).unwrap();published(&file);assert!(matches!(readable(file.as_raw_fd()),Err(errno::EBADF)));close(file);
        let file=File::open(&path).unwrap();super::super::insert(file.as_raw_fd(),Kind::Path(super::super::PathDescription::new(super::super::super::fs::O_PATH)));published(&file);assert!(matches!(readable(file.as_raw_fd()),Err(errno::EBADF)));close(file);
        let file=File::open(&path).unwrap();super::super::insert(file.as_raw_fd(),Kind::Random);published(&file);assert!(matches!(readable(file.as_raw_fd()),Err(errno::EOPNOTSUPP)));close(file);
        let file=File::open(&path).unwrap();published(&file);
        let mut pin=super::super::pin_guest(file.as_raw_fd()).unwrap();
        pin.fuse=Some(std::sync::Arc::new(super::super::super::fuse_client::Open{guest:"/unit/fuse".into(),route:crate::vfs::FuseRoute{session:root.join("no-broker-needed"),relative:String::new(),uid:0,gid:0,allow_other:false,default_permissions:false,read_only:true},node:1,generation:0,fh:0,flags:0,open_flags:0,broker_owned:true,directory:false,offset:std::sync::Mutex::new(0)}));
        assert!(matches!(validate(pin),Err(errno::EOPNOTSUPP)));close(file);
        let file=File::open(&path).unwrap();
        let store=std::sync::Arc::new(aim_storage::fsverity::Store::new(&root.join("native-proofs"),&root.join("native-locks")).unwrap());
        let description=super::super::super::regular_file::Description::new(file.as_fd(),0,store).unwrap();
        super::super::insert(file.as_raw_fd(),Kind::Regular(description));published(&file);assert!(matches!(readable(file.as_raw_fd()),Err(errno::EOPNOTSUPP)));close(file);
        let directory=File::open(path.parent().unwrap()).unwrap();published(&directory);assert!(matches!(readable(directory.as_raw_fd()),Err(errno::EOPNOTSUPP)));close(directory);
        assert!(LEASES.with(|leases|leases.borrow().is_empty()));
    }
}
