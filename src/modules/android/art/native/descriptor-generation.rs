// Called only by a producer that just received a new native descriptor slot.
// This retires the previous numeric generation, never a live descriptor based
// on its fstat type. Retained pins keep their separate hidden open description.
pub(crate) fn begin_fresh(fd:i32)->Result<(),crate::errno::Errno>{
    if RETIRED.with(|owners|owners.borrow().is_some()){return begin_fresh_locked(fd);}
    let _guard=lifecycle();begin_fresh_locked(fd)
}
fn begin_fresh_locked(fd:i32)->Result<(),crate::errno::Errno>{
    if is_hidden(fd){return Err(crate::errno::EBADF);}
    if visible(fd){withdraw_guest(fd)?;}
    descriptor_pins::retire(fd);
    super::fuse_client::close(fd);
    let previous=TABLE.write().unwrap().remove(&fd);
    // on_close's Content recycling applies to the old native description.
    // The slot already names a new one here, so do not recycle or close it.
    retire_entry(previous);set_slow(fd,true);Ok(())
}
