//! Native ABI over the original AIM executable-memory owner. VM executors keep
//! their own existing syscall, signal and thread-context ownership.

struct NativeErrno(i32);
impl NativeErrno {
    fn save() -> Self { Self(unsafe { *libc::__error() }) }
}
impl Drop for NativeErrno {
    fn drop(&mut self) { unsafe { *libc::__error() = self.0; } }
}

/// Original AIM MRS/MSR TP emission with the surrounding executor's allocated
/// Darwin key. The caller owns the branch back and the image's veneer arena.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn klj_guest_thread_pointer_stub(
    instruction: u32, slot_offset: u32, shadow_offset: u32, output: *mut u32, capacity: usize,
) -> i64 {
    use crate::a64::{self,Kind};
    let Some((kind,register))=a64::classify(instruction) else { return -(crate::errno::EINVAL as i64); };
    if !matches!(kind,Kind::MrsTp|Kind::MsrTp) || slot_offset&7!=0 || slot_offset>32760
        || shadow_offset&7!=0 || shadow_offset>32760 || output.is_null() {
        return -(crate::errno::EINVAL as i64);
    }
    let body=a64::stub_body_with_tp_slots(kind,register,0,slot_offset,shadow_offset);
    if body.len()>capacity { return -(crate::errno::ENOSPC as i64); }
    unsafe { std::ptr::copy_nonoverlapping(body.as_ptr(),output,body.len()); }
    body.len() as i64
}

/// The caller holds its physical mapping-mutation lock. These are Darwin's
/// original R/W/X bits, shared by the Android UAPI; no guest flags are invented.
#[unsafe(no_mangle)]
pub extern "C" fn klj_guest_executable_protect(address: usize, length: usize, protection: i32) -> i64 {
    klj_guest_executable_protect_prepared(address,length,protection,None,None)
}

#[unsafe(no_mangle)]
pub extern "C" fn klj_guest_executable_protect_prepared(address:usize,length:usize,protection:i32,
    prepare:Option<unsafe extern "C" fn(usize,*mut libc::c_void,usize)->i64>,
    finish:Option<unsafe extern "C" fn(usize,*mut libc::c_void,usize,i32)>)->i64 {
    let _errno = NativeErrno::save();
    let page = super::mem::PAGE as usize;
    if address % page != 0 || length.checked_add(page - 1).is_none() {
        return -(crate::errno::EINVAL as i64);
    }
    let length = (length + page - 1) & !(page - 1);
    if address.checked_add(length).is_none() { return -(crate::errno::EINVAL as i64); }
    if length == 0 { return 0; }
    if protection & (libc::PROT_WRITE | libc::PROT_EXEC) == libc::PROT_WRITE | libc::PROT_EXEC {
        super::jit::protect_rwx(address as u64, length as u64)
    } else if protection & libc::PROT_EXEC != 0 && prepare.is_some() {
        super::jit::protect_around_prepared(address as u64,length as u64,protection,prepare,finish)
    } else {
        // Keep ordinary data protections on Darwin's direct path. Only its
        // MAP_JIT transition refusal needs the original migration owner.
        let result=unsafe { libc::mprotect(address as *mut _,length,protection) };
        if result==0 { 0 } else {
            let error=crate::errno::last();
            if error==crate::errno::EACCES {
                super::jit::protect_around(address as u64,length as u64,protection)
            } else { -(error as i64) }
        }
    }
}

/// The kernel tag identifies this executor's JIT pages without an address
/// registry or mistaking HotSpot's own JIT cache for Android-generated code.
#[unsafe(no_mangle)]
pub extern "C" fn klj_guest_executable_contains(address: usize) -> i32 {
    let _errno = NativeErrno::save();
    super::vmmap::info_at(address as u64).is_some_and(|region|
        region.start <= address as u64 && (address as u64) < region.end
        && region.tag == libc::VM_MEMORY_APPLICATION_SPECIFIC_1 as u32
        && region.prot & 7 == 7) as i32
}

#[unsafe(no_mangle)]
pub extern "C" fn klj_guest_executable_context_size() -> usize {
    std::mem::size_of::<crate::context::GuestContext>()
}

#[unsafe(no_mangle)]
pub extern "C" fn klj_guest_executable_context_alignment() -> usize {
    std::mem::align_of::<crate::context::GuestContext>()
}

/// Borrow the original context's native stack cursor and host-execution flag.
/// Their layout stays with AIM rather than being copied into VM assembly.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn klj_guest_executable_native_stack(
    storage: *mut libc::c_void, cursor: *mut *mut u64, in_host: *mut *mut u32,
) {
    let context = storage.cast::<crate::context::GuestContext>();
    unsafe {
        cursor.write(std::ptr::addr_of_mut!((*context).host_sp));
        in_host.write(std::ptr::addr_of_mut!((*context).in_host));
    }
}

/// Storage and the native resume stack remain owned by the physical thread.
/// Unlike context::init_thread(), this does not claim AIM's fixed TSD slots,
/// register another thread record or replace the VM's alternate signal stack.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn klj_guest_executable_context_init(
    storage: *mut libc::c_void, storage_size: usize, stack_start: usize, stack_size: usize,
) -> i64 {
    let Some(stack_end) = stack_start.checked_add(stack_size) else { return -(crate::errno::EINVAL as i64); };
    if storage.is_null() || storage as usize % klj_guest_executable_context_alignment() != 0
        || storage_size < klj_guest_executable_context_size() || (stack_end & !15) <= stack_start {
        return -(crate::errno::EINVAL as i64);
    }
    // SAFETY: the executor supplies aligned thread-owned storage. The original
    // GuestContext::zeroed() uses this same all-zero initial representation.
    let mut context: crate::context::GuestContext = unsafe { std::mem::zeroed() };
    context.host_sp = (stack_end & !15) as u64;
    unsafe { storage.cast::<crate::context::GuestContext>().write(context); }
    0
}

/// The existing physical dispatcher supplies its original Darwin signal
/// context. Original AIM code owns store emulation, write/execute transitions
/// and register conversion. Foreign/VM JIT pages retain their original owner.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn klj_guest_executable_fault(
    storage: *mut libc::c_void, signal: i32, code: i32, ucontext: *mut libc::c_void,
) -> i32 {
    let _errno = NativeErrno::save();
    if storage.is_null() || ucontext.is_null() || code <= 0 || code >= 0x10000 { return 0; }
    if !matches!(signal, libc::SIGBUS | libc::SIGSEGV | libc::SIGILL) { return 0; }
    // SAFETY: storage is this physical thread's initialized opaque context;
    // ucontext and its mcontext belong to the native kernel signal callback.
    let context = unsafe { &mut *storage.cast::<crate::context::GuestContext>() };
    let machine = unsafe { super::sigframe::mcontext(ucontext) };
    if signal == libc::SIGILL && machine.pc == crate::context::resume_trap_pc() && context.in_host != 0 {
        super::sigframe::Cpu::from_ctx(context).to_mc(machine);
        context.in_host = 0;
        return 1;
    }
    if !matches!(signal, libc::SIGBUS | libc::SIGSEGV) { return 0; }
    // The original execute-resume thunk only changes this thread's JIT view
    // and enters the original resume trap consumed above. It needs no fixed
    // TLS slot when the surrounding executor already owns this context.
    super::jit::fault_with_owner(context, machine,
        |address| klj_guest_executable_contains(address as usize) != 0) as i32
}

/// The callback's storage stays on its native caller's stack. No Rust frame
/// spans an APK handler, preserving native nonlocal returns and unwinding.
#[repr(C)]
pub struct SignalFrame {
    pub frame: *mut libc::c_void,
    pub info: *mut libc::c_void,
    pub context: *mut libc::c_void,
}

#[unsafe(no_mangle)]
pub extern "C" fn klj_guest_signal_frame_size() -> usize {
    super::sigframe::FRAME_SIZE as usize + 32
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn klj_guest_signal_frame_prepare(
    storage: *mut libc::c_void, size: usize, linux_siginfo: *const libc::c_void,
    ucontext: *mut libc::c_void, mask: u64, output: *mut SignalFrame,
) -> i64 {
    let _errno=NativeErrno::save();
    if storage.is_null() || storage as usize & 15 != 0 || size < klj_guest_signal_frame_size()
        || linux_siginfo.is_null() || ucontext.is_null() || output.is_null() {
        return -(crate::errno::EINVAL as i64);
    }
    let top=storage as u64 + size as u64;
    let machine=unsafe { super::sigframe::mcontext(ucontext) };
    let cpu=super::sigframe::Cpu::from_mc(machine);
    let native=unsafe { &*(ucontext as *const libc::ucontext_t) };
    let alt=super::sigframe::AltStack {
        sp:native.uc_stack.ss_sp as u64,size:native.uc_stack.ss_size as u64,
        flags:if native.uc_stack.ss_flags&libc::SS_DISABLE!=0 { super::sigframe::SS_DISABLE }
            else if native.uc_stack.ss_flags&libc::SS_ONSTACK!=0 { super::sigframe::SS_ONSTACK } else { 0 },
    };
    let action=super::sigframe::KSigaction {handler:0,flags:super::sigframe::SA_SIGINFO,restorer:0,mask:0};
    let info=unsafe { &*linux_siginfo.cast::<super::sigframe::Siginfo>() };
    let delivery=super::sigframe::Delivery { info,act:&action,uc_mask:mask,esr:machine.esr as u64,
        fault_address:machine.far,default_restorer:0 };
    let entry=match unsafe { super::sigframe::setup_on_stack(&cpu,&delivery,&alt,top) } {Ok(entry)=>entry,Err(error)=>return -(error as i64)};
    unsafe { output.write(SignalFrame {frame:entry.sp as *mut _,info:entry.x1 as *mut _,context:entry.x2 as *mut _}); }
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn klj_guest_signal_frame_restore(
    frame: *const libc::c_void, ucontext: *mut libc::c_void, returned_mask: *mut u64,
) -> i64 {
    let _errno=NativeErrno::save();
    if frame.is_null() || ucontext.is_null() { return -(crate::errno::EINVAL as i64); }
    if let Some((cpu,mask,_))=unsafe { super::sigframe::restore_native(frame as u64) } {
        cpu.to_mc(unsafe { super::sigframe::mcontext(ucontext) });
        if !returned_mask.is_null() { unsafe { returned_mask.write(mask); } }
        0
    } else { -(crate::errno::EINVAL as i64) }
}

/// A Linux handler may chain a saved native disposition with its public
/// ucontext, including a copy. Keep Linux offsets inside the original owner.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn klj_guest_signal_context_restore(
    context: *const libc::c_void, ucontext: *mut libc::c_void, returned_mask: *mut u64,
) -> i64 {
    let _errno=NativeErrno::save();
    if context.is_null() || ucontext.is_null() { return -(crate::errno::EINVAL as i64); }
    let frame=(context as u64).wrapping_sub(super::sigframe::UC);
    if let Some((cpu,mask,alt))=unsafe { super::sigframe::restore(frame) } {
        let machine=unsafe { super::sigframe::mcontext(ucontext) };
        cpu.to_mc(machine);
        let (far,esr)=unsafe { super::sigframe::chained_context_fault(context as u64) };
        machine.far=far;machine.esr=esr;
        let native=unsafe { &mut *ucontext.cast::<libc::ucontext_t>() };
        native.uc_stack.ss_sp=alt.sp as *mut _;
        native.uc_stack.ss_size=alt.size as usize;
        native.uc_stack.ss_flags=if alt.flags&super::sigframe::SS_DISABLE!=0 { libc::SS_DISABLE }
            else if alt.flags&super::sigframe::SS_ONSTACK!=0 { libc::SS_ONSTACK } else { 0 };
        if !returned_mask.is_null() { unsafe { returned_mask.write(mask); } }
        0
    } else { -(crate::errno::EINVAL as i64) }
}

/// Return native register/mask edits through the original frame's layout while
/// retaining the caller's flags/link, fault fields and extension records.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn klj_guest_signal_context_update(
    context: *mut libc::c_void, ucontext: *mut libc::c_void, mask: u64,
) -> i64 {
    let _errno=NativeErrno::save();
    if context.is_null() || ucontext.is_null() { return -(crate::errno::EINVAL as i64); }
    let cpu=super::sigframe::Cpu::from_mc(unsafe { super::sigframe::mcontext(ucontext) });
    unsafe { super::sigframe::update_chained_context(context as u64,&cpu,mask); }
    0
}

/// Register/query the original process-wide Linux disposition owner. Unlike
/// delivery, rt_sigaction needs no executor-specific thread or TLS context.
#[unsafe(no_mangle)]
pub extern "C" fn klj_guest_signal_action(
    signal: i32, action: *const super::sigframe::KSigaction,
    previous: *mut super::sigframe::KSigaction,
) -> i64 {
    let _errno=NativeErrno::save();
    super::signal::rt_sigaction([signal as u64,action as u64,previous as u64,8,0,0])
}
