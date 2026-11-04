//! Preserve existing open descriptions when a guest lowers its physical limit.
//! Normal descriptors acquire no additional retained alias. Only a limit
//! reduction creates these owners, under the existing publication/close lock.
use super::*;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};

static OWNERS: LazyLock<Mutex<HashMap<i32, Arc<Owner>>>> = LazyLock::new(Default::default);
thread_local! { static RETIRED: std::cell::RefCell<Option<Vec<Arc<Owner>>>> = const { std::cell::RefCell::new(None) }; }

pub(super) struct Owner {
    descriptor: Option<OwnedFd>,
    kind: Option<Kind>,
    socket_allowed: bool,
    fuse: Option<Arc<super::super::fuse_client::Open>>,
}
impl Owner { pub(super) fn fd(&self) -> i32 { self.descriptor.as_ref().unwrap().as_raw_fd() } }
impl Drop for Owner {
    fn drop(&mut self) {
        let guard = lifecycle();
        let fd = self.fd();
        if let Some(file) = &self.fuse { super::super::fuse_client::remove_private_alias(fd, file); }
        let (previous, last_socket) = {
            let mut table = TABLE.write().unwrap();
            let previous = table.remove(&fd);
            let last = match &self.kind {
                Some(Kind::Sock(socket)) => !table.values().any(|entry| matches!(&entry.kind, Some(Kind::Sock(other)) if Arc::ptr_eq(socket,other))),
                _ => false,
            };
            (previous,last)
        };
        retire_entry(previous);
        drop(self.descriptor.take());
        unhide(fd); super::super::fd_visibility::private_closed(fd);
        drop(guard);
        if last_socket { super::super::close_effects::note_socket_close(); }
    }
}

pub(super) fn begin() { RETIRED.with(|owners| { assert!(owners.borrow().is_none()); *owners.borrow_mut() = Some(Vec::new()); }); }
pub(super) fn end() -> Vec<Arc<Owner>> { RETIRED.with(|owners| owners.borrow_mut().take().unwrap()) }
pub(super) fn retire(fd:i32) {
    let mut owner=OWNERS.lock().unwrap().remove(&fd);
    RETIRED.with(|retired| { if let Some(retired)=retired.borrow_mut().as_mut() { if let Some(owner)=owner.take() { retired.push(owner); } } });
    drop(owner);
}
pub(super) fn pin(fd:i32)->Result<Option<Pinned>,crate::errno::Errno> {
    let _guard=lifecycle();
    require_guest_visible(fd)?;
    let owner=OWNERS.lock().unwrap().get(&fd).cloned();
    Ok(owner.map(|owner| Pinned { descriptor:None, kind:owner.kind.clone(), socket_allowed:owner.socket_allowed,
        fuse:owner.fuse.clone(), borrowed:None,regular_access:None, retained:Some(owner) }))
}

fn capture(fd:i32)->Result<Arc<Owner>,crate::errno::Errno> {
    require_guest_visible(fd)?;
    let mut stat:libc::stat=unsafe{std::mem::zeroed()};
    if unsafe{libc::fstat(fd,&mut stat)}<0 { return Err(crate::errno::last()); }
    let kind=get(fd); let fuse=super::super::fuse_client::get(fd);
    let socket_allowed=stat.st_mode&libc::S_IFMT==libc::S_IFSOCK&&matches!(kind,None|Some(Kind::Sock(_)))
        && !super::super::fuse_device::is_device(fd)&&fuse.is_none();
    let mut alias=unsafe{libc::fcntl(fd,libc::F_DUPFD_CLOEXEC,hidden_base())};
    // An already lower inherited limit may have no high slots. The existing
    // private registrar also permits a low, explicitly hidden descriptor.
    if alias<0 { alias=unsafe{libc::fcntl(fd,libc::F_DUPFD_CLOEXEC,0)}; }
    if alias<0 { return Err(crate::errno::last()); }
    let alias=hide(alias);
    if let Err(error)=super::super::fd_visibility::hide(alias) {
        unhide(alias);unsafe{libc::close(alias);}return Err(error);
    }
    if let Some(file)=&fuse {
        if let Err(error)=super::super::fuse_client::install_private_alias(alias,file.clone()) {
            unhide(alias);unsafe{libc::close(alias);}super::super::fd_visibility::private_closed(alias);return Err(error);
        }
    }
    let entry=TABLE.read().unwrap().get(&fd).cloned();
    if let Some(entry)=entry { TABLE.write().unwrap().insert(alias,entry); set_slow(alias,true); }
    // Force the owned close/replacement path even if a future lean path is
    // restored for ordinary plain descriptors.
    TABLE.write().unwrap().entry(fd).or_default(); set_slow(fd,true);
    Ok(Arc::new(Owner { descriptor:Some(unsafe{OwnedFd::from_raw_fd(alias)}),kind,socket_allowed,fuse }))
}

/// Host acceptance precedes the credential owner's advertised-limit commit.
/// Failed host changes leave only valid private aliases, never changed limits.
pub(crate) fn change(soft:u64,apply:impl FnOnce()->i64)->i64 {
    let _guard=lifecycle();
    let mut current:libc::rlimit=unsafe{std::mem::zeroed()};
    if unsafe{libc::getrlimit(libc::RLIMIT_NOFILE,&mut current)}<0 { return -(crate::errno::last() as i64); }
    if soft<current.rlim_cur {
        for fd in super::super::fd_visibility::visible() {
            if OWNERS.lock().unwrap().contains_key(&fd) { continue; }
            let owner=match capture(fd) { Ok(owner)=>owner,Err(error)=>return -(error as i64) };
            OWNERS.lock().unwrap().insert(fd,owner);
        }
    }
    apply()
}

pub(super) fn exec_records()->Result<Vec<String>,crate::errno::Errno> {
    let mut records=Vec::new();
    for (fd,owner) in OWNERS.lock().unwrap().iter() {
        if !visible(*fd) { continue; }
        let flags=unsafe{libc::fcntl(*fd,libc::F_GETFD)};
        if flags<0 { return Err(crate::errno::last()); }
        if flags&libc::FD_CLOEXEC==0 { records.push(format!("limit-pin\t{fd}\t{}",owner.fd())); }
    }
    Ok(records)
}
#[derive(Default)]
pub(super) struct ExecFlags(Vec<(Arc<Owner>,i32)>);
impl Drop for ExecFlags {
    fn drop(&mut self) { for (owner,flags) in &self.0 { unsafe{libc::fcntl(owner.fd(),libc::F_SETFD,*flags);} } }
}
pub(super) fn prepare_exec()->Result<ExecFlags,crate::errno::Errno> {
    let mut changed=ExecFlags::default();
    for (fd,owner) in OWNERS.lock().unwrap().iter() {
        if !visible(*fd) { continue; }
        let guest_flags=unsafe{libc::fcntl(*fd,libc::F_GETFD)};
        if guest_flags<0 { return Err(crate::errno::last()); }
        if guest_flags&libc::FD_CLOEXEC!=0 { continue; }
        let flags=unsafe{libc::fcntl(owner.fd(),libc::F_GETFD)};
        if flags<0||unsafe{libc::fcntl(owner.fd(),libc::F_SETFD,flags&!libc::FD_CLOEXEC)}<0 { return Err(crate::errno::last()); }
        changed.0.push((owner.clone(),flags));
    }
    Ok(changed)
}
pub(super) fn restore_exec(records:&[(i32,i32)])->Result<(),crate::errno::Errno> {
    let _guard=lifecycle();
    let mut restored=std::collections::HashSet::new();
    for &(fd,alias) in records {
        require_guest_visible(fd)?;
        if alias<0||alias==fd||visible(alias)||!restored.insert(alias)||OWNERS.lock().unwrap().contains_key(&fd) { return Err(crate::errno::EBADF); }
        let flags=unsafe{libc::fcntl(alias,libc::F_GETFD)};
        if flags<0||unsafe{libc::fcntl(alias,libc::F_SETFD,flags|libc::FD_CLOEXEC)}<0 { return Err(crate::errno::last()); }
        super::super::fd_visibility::hide(alias)?; keep_hidden(alias);
        let kind=get(fd);let fuse=super::super::fuse_client::get(fd);
        let mut stat:libc::stat=unsafe{std::mem::zeroed()};
        if unsafe{libc::fstat(alias,&mut stat)}<0 { return Err(crate::errno::last()); }
        let socket_allowed=stat.st_mode&libc::S_IFMT==libc::S_IFSOCK&&matches!(kind,None|Some(Kind::Sock(_)))
            && !super::super::fuse_device::is_device(fd)&&fuse.is_none();
        if let Some(file)=&fuse { super::super::fuse_client::install_private_alias(alias,file.clone())?; }
        let entry=TABLE.read().unwrap().get(&fd).cloned();
        if let Some(entry)=entry { TABLE.write().unwrap().insert(alias,entry);set_slow(alias,true); }
        TABLE.write().unwrap().entry(fd).or_default();set_slow(fd,true);
        OWNERS.lock().unwrap().insert(fd,Arc::new(Owner { descriptor:Some(unsafe{OwnedFd::from_raw_fd(alias)}),kind,socket_allowed,fuse }));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn lowered_limits_keep_pins_through_close_replace_and_exec_flags() {
        if isolated_kernel_test("sys::fdtab::descriptor_pins::tests::lowered_limits_keep_pins_through_close_replace_and_exec_flags") { return; }
        let (_view,_root)=crate::vfs::test_view();install_storage_registrar().unwrap();
        let mut first=[0;2];let mut second=[0;2];
        assert_eq!(unsafe{libc::pipe(first.as_mut_ptr())},0);assert_eq!(unsafe{libc::pipe(second.as_mut_ptr())},0);
        {let _guard=lifecycle();for fd in first.into_iter().chain(second) {publish_guest(fd).unwrap();}}
        let apply=||{let limit=libc::rlimit{rlim_cur:64,rlim_max:64};assert_eq!(unsafe{libc::setrlimit(libc::RLIMIT_NOFILE,&limit)},0);0};
        assert_eq!(change(64,apply),0);
        let held=pin_guest(first[0]).unwrap();let alias=held.descriptor().as_raw_fd();
        assert!(is_hidden(alias)&&!visible(alias));
        let flags=unsafe{libc::fcntl(alias,libc::F_GETFD)};assert_ne!(flags&libc::FD_CLOEXEC,0);
        assert!(exec_records().unwrap().iter().any(|record|record==&format!("limit-pin\t{}\t{alias}",first[0])));
        {let _flags=prepare_exec().unwrap();assert_eq!(unsafe{libc::fcntl(alias,libc::F_GETFD)}&libc::FD_CLOEXEC,0);}
        assert_eq!(unsafe{libc::fcntl(alias,libc::F_GETFD)},flags);
        assert_eq!(unsafe{libc::fcntl(first[0],libc::F_SETFD,libc::FD_CLOEXEC)},0);
        assert!(!exec_records().unwrap().iter().any(|record|record.starts_with(&format!("limit-pin\t{}\t",first[0]))));
        {let _flags=prepare_exec().unwrap();assert_ne!(unsafe{libc::fcntl(alias,libc::F_GETFD)}&libc::FD_CLOEXEC,0);}
        let (ready,waiting)=std::sync::mpsc::channel();
        let worker=std::thread::spawn(move||{ready.send(()).unwrap();let mut byte=0u8;assert_eq!(unsafe{libc::read(held.descriptor().as_raw_fd(),(&mut byte as *mut u8).cast(),1)},1);byte});
        waiting.recv().unwrap();
        assert_eq!(super::super::super::fs::dup3([second[0]as u64,first[0]as u64,0,0,0,0]),first[0]as i64);
        let current=pin_guest(first[0]).unwrap();assert_ne!(current.descriptor().as_raw_fd(),alias);
        assert_eq!(unsafe{libc::write(first[1],b"a".as_ptr().cast(),1)},1);assert_eq!(worker.join().unwrap(),b'a');
        assert_eq!(unsafe{libc::fcntl(alias,libc::F_GETFD)},-1);assert_eq!(std::io::Error::last_os_error().raw_os_error(),Some(libc::EBADF));
        assert_eq!(unsafe{libc::write(second[1],b"b".as_ptr().cast(),1)},1);
        let mut byte=0;assert_eq!(unsafe{libc::read(current.descriptor().as_raw_fd(),(&mut byte as *mut u8).cast(),1)},1);assert_eq!(byte,b'b');drop(current);
        for fd in first.into_iter().chain(second) {assert_eq!(super::super::super::fs::close([fd as u64,0,0,0,0,0]),0);}
        assert!(OWNERS.lock().unwrap().keys().all(|fd|!first.contains(fd)&&!second.contains(fd)));
    }
}
