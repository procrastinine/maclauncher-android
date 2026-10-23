/// Physical host state borrowed only while original guest code is running.
/// Its stack, signal mask, errno and all four AIM keys return to HotSpot before
/// the native method returns. The guest record/TLS remain dormant, not rebound
/// to another OS thread.
pub(crate) struct ForeignHostBinding {
    slots:[u64;4],
    alternate:libc::stack_t,
    mask:libc::sigset_t,
    errno:i32,
    jit_protected:bool,
    previous_jit_view:Option<bool>,
    restored:bool,
}
impl ForeignHostBinding {
    pub(crate) fn capture()->Result<Self,i64>{
        claim_slots();
        let mut alternate:libc::stack_t=unsafe{std::mem::zeroed()};
        let mut mask:libc::sigset_t=unsafe{std::mem::zeroed()};
        let errno=unsafe{*libc::__error()};
        if unsafe{libc::sigaltstack(std::ptr::null(),&mut alternate)}!=0{return Err(-(crate::errno::last() as i64));}
        if alternate.ss_flags&libc::SS_ONSTACK!=0{return Err(-(crate::errno::EBUSY as i64));}
        let error=unsafe{libc::pthread_sigmask(libc::SIG_SETMASK,std::ptr::null(),&mut mask)};
        if error!=0{return Err(-(crate::errno::from_darwin(error)as i64));}
        let slots=[slot::CTX,slot::TP,slot::ENTRY,slot::SCS].map(|offset|unsafe{slot_ptr(offset).read()});
        let jit_protected=physical_jit_view().map_err(|error|-(error as i64))?;
        let previous_jit_view=FOREIGN_JIT_VIEW.with(|value|value.replace(Some(jit_protected)));
        Ok(Self{slots,alternate,mask,errno,jit_protected,previous_jit_view,restored:false})
    }
    pub(crate) fn resume_native_mask(&self)->Result<(),i64>{let error=unsafe{libc::pthread_sigmask(libc::SIG_SETMASK,&self.mask,std::ptr::null_mut())};if error==0{Ok(())}else{Err(-(crate::errno::from_darwin(error)as i64))}}
    fn restore_inner(&self)->Result<(),i64>{
        let mut all:libc::sigset_t=unsafe{std::mem::zeroed()};unsafe{libc::sigfillset(&mut all);}
        let block=unsafe{libc::pthread_sigmask(libc::SIG_BLOCK,&all,std::ptr::null_mut())};
        if block!=0{return Err(-(crate::errno::from_darwin(block)as i64));}
        for(offset,value)in [slot::CTX,slot::TP,slot::ENTRY,slot::SCS].into_iter().zip(self.slots){unsafe{slot_ptr(offset).write(value);}}
        // Darwin requires a minimum size even for some SS_DISABLE requests.
        // The disabled pointer/size are ignored by POSIX; never activate them.
        let mut alternate=self.alternate;
        if alternate.ss_flags&libc::SS_DISABLE!=0&&alternate.ss_size<libc::MINSIGSTKSZ{alternate.ss_size=libc::MINSIGSTKSZ;}
        let stack_error=if unsafe{libc::sigaltstack(&alternate,std::ptr::null_mut())}!=0{crate::errno::last()}else{0};
        set_physical_jit_view(self.jit_protected);
        FOREIGN_JIT_VIEW.with(|value|value.set(self.previous_jit_view));
        let mask_error=unsafe{libc::pthread_sigmask(libc::SIG_SETMASK,&self.mask,std::ptr::null_mut())};
        unsafe{*libc::__error()=self.errno;}
        if stack_error!=0{eprintln!("admission: restore alternate stack failed errno={stack_error}");return Err(-(stack_error as i64));}
        if mask_error!=0{return Err(-(crate::errno::from_darwin(mask_error)as i64));}
        Ok(())
    }
}
// HotSpot caches its requested WX state. Original guest JIT operations change
// the physical Darwin view independently, so every returning boundary restores
// that view without accessing or changing the VM's private thread state.
thread_local!{static FOREIGN_JIT_VIEW:std::cell::Cell<Option<bool>>=const{std::cell::Cell::new(None)};}
fn physical_jit_view()->Result<bool,crate::errno::Errno>{
 let saved=unsafe{*libc::__error()};let result=crate::sys::jit_user_copy::protected();unsafe{*libc::__error()=saved;}result
}
fn set_physical_jit_view(protected:bool){
 let saved=unsafe{*libc::__error()};unsafe{libc::pthread_jit_write_protect_np(i32::from(protected));*libc::__error()=saved;}
}
pub(crate) struct ForeignJitView{restore:Option<bool>}
impl ForeignJitView{
 fn capture()->Result<Self,crate::errno::Errno>{
  if FOREIGN_JIT_VIEW.with(|value|value.get().is_none()){return Ok(Self{restore:None});}
  Ok(Self{restore:Some(physical_jit_view()?)})
 }
 pub(crate) fn host()->Result<Self,crate::errno::Errno>{
  let guard=Self::capture()?;if let Some(view)=FOREIGN_JIT_VIEW.with(|value|value.get()){set_physical_jit_view(view);}Ok(guard)
 }
}
impl Drop for ForeignJitView{fn drop(&mut self){if let Some(view)=self.restore{set_physical_jit_view(view);}}}
thread_local!{static FOREIGN_RESTORE_ERROR:std::cell::Cell<i64>=const{std::cell::Cell::new(0)};}
pub(crate) fn take_foreign_restore_error()->i64{FOREIGN_RESTORE_ERROR.with(|value|value.replace(0))}
impl ForeignHostBinding{pub(crate) fn restore(mut self)->Result<(),i64>{let result=self.restore_inner();if result.is_ok(){self.restored=true;}result}}
impl Drop for ForeignHostBinding{fn drop(&mut self){if !self.restored{if let Err(error)=self.restore_inner(){FOREIGN_RESTORE_ERROR.with(|value|value.set(error));}}}}
pub(crate) unsafe fn activate_foreign(ctx:*mut GuestContext,tp:u64,scs:u64,stacks:&HostStacks)->Result<(),i64>{unsafe{activate_foreign_with_size(ctx,tp,scs,stacks,ALTSTACK_SIZE)}}
pub(crate) unsafe fn activate_foreign_with_size(ctx:*mut GuestContext,tp:u64,scs:u64,stacks:&HostStacks,alternate_size:usize)->Result<(),i64>{
    let mut all:libc::sigset_t=unsafe{std::mem::zeroed()};unsafe{libc::sigfillset(&mut all);}
    let error=unsafe{libc::pthread_sigmask(libc::SIG_BLOCK,&all,std::ptr::null_mut())};
    if error!=0{return Err(-(crate::errno::from_darwin(error)as i64));}
    unsafe{
        slot_ptr(slot::CTX).write(ctx as u64);slot_ptr(slot::TP).write(tp);
        slot_ptr(slot::ENTRY).write(linux_abi_syscall_entry as usize as u64);slot_ptr(slot::SCS).write(scs);
        let ss=libc::stack_t{ss_sp:stacks.altstack()as *mut _,ss_size:alternate_size,ss_flags:0};
        if libc::sigaltstack(&ss,std::ptr::null_mut())!=0{return Err(-(crate::errno::last()as i64));}
        (*ctx).in_host=1;
    }
    // Original pending-signal delivery occurs through linux_abi_resume; the
    // original guest signal mask remains in the original ThreadSignals owner.
    Ok(())
}

pub(crate) unsafe fn foreign_stacks(ctx:*mut GuestContext)->HostStacks{crate::sys::initialize_verity_fault_slot();claim_slots();let stacks=HostStacks::map();unsafe{(*ctx).owned_scs_base=stacks.scs();}stacks}
pub(crate) fn foreign_alternate_size(reject:bool)->usize{if reject{1}else{ALTSTACK_SIZE}}

