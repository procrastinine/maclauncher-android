// Included in the original AIM diagnostic module. Ordinary handled ART faults
// only copy their interrupted register array into that module's existing slot.
// By default memory reads and formatting happen solely on a fatal-report path.
// An explicit launch diagnostic can also save the interrupted SIGILL word;
// ordinary ART page faults still perform no diagnostic reads.

// A separate opt-in preserves a small number of handled protection faults
// before another guest thread can exit the process. This is a diagnostic
// budget, not an owner or a history of faults.
static HANDLED_FAULTS: std::sync::atomic::AtomicU32 =
    std::sync::atomic::AtomicU32::new(0);
const HANDLED_FAULT_LIMIT: u32 = 16;

// Original --trace can attribute an unmap beyond a shared libc wrapper. This
// uses the original guest-memory owner, bounded to eight records and invoked
// only for an explicitly traced munmap entry.
pub(crate) fn trace_frame_returns(mut fp:u64)->String {
    let _errno=FaultReportErrno::save();let mut text=String::new();
    for index in 0..8 {
        if fp==0||fp%16!=0{break;}
        let Ok(bytes)=crate::sys::user_memory::read_exact(fp,16)else{break;};
        let record=[u64::from_ne_bytes(bytes[..8].try_into().unwrap()),u64::from_ne_bytes(bytes[8..].try_into().unwrap())];
        if record[1]==0{break;}
        use std::fmt::Write;
        let _=write!(text,"#{index}={:#x} ",record[1]&0x00ff_ffff_ffff_ffff);
        if record[0]<=fp{break;}fp=record[0];
    }
    text
}

pub fn configure_handled_fault_capture(enabled: bool) {
    HANDLED_FAULTS.store(if enabled { HANDLED_FAULT_LIMIT } else { 0 },
        std::sync::atomic::Ordering::Relaxed);
}

// No allocation, symbol lookup, guest call, formatting lock or faulting
// pointer dereference is allowed before the original guest handler runs.
struct RawFaultLine {
    bytes: [u8; 2048],
    len: usize,
}
impl RawFaultLine {
    fn add(&mut self, value: &[u8]) {
        let count = value.len().min(self.bytes.len() - self.len);
        self.bytes[self.len..self.len + count].copy_from_slice(&value[..count]);
        self.len += count;
    }
    fn hex(&mut self, value: u64) {
        self.add(b"0x");
        for shift in (0..16).rev() {
            self.add(&[b"0123456789abcdef"[((value >> (shift * 4)) & 15) as usize]]);
        }
    }
    fn field(&mut self, name: &[u8], value: u64) {
        self.add(name); self.hex(value);
    }
}

pub fn report_handled_fault(fault: &Fault, linux_code: i32, guest_tid: i32) {
    // The code is already translated by the original Linux sigframe owner.
    // ART's ordinary implicit-null faults (SEGV_MAPERR) remain silent.
    if fault.sig != libc::SIGSEGV || linux_code != 2 { return; }
    let Ok(remaining) = HANDLED_FAULTS.fetch_update(
        std::sync::atomic::Ordering::Relaxed,
        std::sync::atomic::Ordering::Relaxed, |value| value.checked_sub(1))
        else { return; };
    let _errno = FaultReportErrno::save();
    let record = (HANDLED_FAULT_LIMIT - remaining) as u64;
    let mut line = RawFaultLine { bytes: [0; 2048], len: 0 };
    line.add(b"[linux-abi] handled protection fault");
    line.field(b" record=", record);
    line.field(b" signal=", fault.sig as u64);
    line.field(b" code=", linux_code as u64);
    line.field(b" guest_tid=", guest_tid as u64);
    // Both functions inspect only this native thread/process's identity.
    line.field(b" host_pid=", unsafe { libc::getpid() } as u64);
    line.field(b" host_pthread=", unsafe { libc::pthread_self() } as u64);
    line.field(b" pc=", fault.pc); line.field(b" lr=", fault.lr);
    line.field(b" fp=", fault.fp); line.field(b" sp=", fault.sp);
    line.field(b" addr=", fault.addr); line.field(b" esr=", fault.esr as u64);
    if let Some(word) = read_fault_word(fault.pc) {
        line.field(b" instruction=", word as u64);
    } else { line.add(b" instruction=unreadable"); }
    for (index, value) in fault.x.iter().enumerate() {
        if index % 4 == 0 { line.add(b"\n[linux-abi] handled registers"); line.field(b" record=", record); }
        line.add(b" x");
        if index >= 10 { line.add(&[b'0' + (index / 10) as u8]); }
        line.add(&[b'0' + (index % 10) as u8]); line.add(b"="); line.hex(*value);
    }
    line.add(b"\n");
    write_raw_fault_line(&line);
}

fn write_raw_fault_line(line: &RawFaultLine) {
    // The existing diagnostics descriptor owns output and retirement. Node's
    // captured streams are sockets: do not wait for capacity or generate
    // SIGPIPE when their reader has retired. A regular log file also permits
    // a raw async-signal-safe write. Other descriptor types are left alone.
    unsafe {
        let fd = log_fd();
        if libc::send(fd, line.bytes.as_ptr().cast(), line.len,
            libc::MSG_DONTWAIT | libc::MSG_NOSIGNAL) < 0 && *libc::__error() == libc::ENOTSOCK {
            let mut status: libc::stat = std::mem::zeroed();
            if libc::fstat(fd, &mut status) == 0 && status.st_mode & libc::S_IFMT == libc::S_IFREG {
                libc::write(fd, line.bytes.as_ptr().cast(), line.len);
            }
        }
    }
}

static ABORT_REQUESTS: std::sync::atomic::AtomicU32 =
    std::sync::atomic::AtomicU32::new(0);

pub fn configure_abort_request_capture(enabled: bool) {
    ABORT_REQUESTS.store(if enabled { 16 } else { 0 },
        std::sync::atomic::Ordering::Relaxed);
}

// Observe only an explicitly requested, validated self-directed abort before
// original signal delivery. Another thread can exit_group before the default
// signal handler reports death. This is a request, not proof of fatal delivery:
// the guest may handle it normally. It never changes signal disposition.
pub fn report_abort_request(sig: i32, target_tid: i32) {
    if sig != libc::SIGABRT || ABORT_REQUESTS.load(std::sync::atomic::Ordering::Relaxed) == 0 {
        return;
    }
    let context = crate::context::current_ctx();
    if context.is_null() { return; }
    let context = unsafe { &*context };
    if context.in_host == 0 || context.tid != target_tid as u64 ||
        !matches!(context.x[8], 130 | 131) { return; }
    if ABORT_REQUESTS.fetch_update(std::sync::atomic::Ordering::Relaxed,
        std::sync::atomic::Ordering::Relaxed, |value| value.checked_sub(1)).is_err() { return; }
    let _errno = FaultReportErrno::save();
    let mut line = RawFaultLine { bytes: [0; 2048], len: 0 };
    line.add(b"[linux-abi] software SIGABRT request");
    line.field(b" guest_tid=", context.tid);
    line.field(b" host_pid=", unsafe { libc::getpid() } as u64);
    line.field(b" host_pthread=", unsafe { libc::pthread_self() } as u64);
    line.field(b" pc=", context.pc);
    line.field(b" stub=", context.stub_ret);
    line.field(b" lr=", context.x[30]);
    line.field(b" fp=", context.x[29]);
    line.field(b" sp=", context.sp);
    let mut fp = context.x[29];
    for index in 0..8 {
        let Some(frame) = frame_record(fp) else { break; };
        line.add(b" return"); line.add(&[b'0' + index]);
        line.add(b"="); line.hex(frame[1] & 0x00ff_ffff_ffff_ffff);
        if frame[0] <= fp { break; }
        fp = frame[0];
    }
    line.add(b"\n");
    // Raw bounded output avoids allocator, symbol/map locks and a stalled or
    // retired diagnostics reader while the original signal is still pending.
    write_raw_fault_line(&line);
}

static CAPTURE_FAULT_INSTRUCTION: std::sync::atomic::AtomicBool =
    std::sync::atomic::AtomicBool::new(false);

pub fn configure_fault_instruction_capture(enabled: bool) {
    CAPTURE_FAULT_INSTRUCTION.store(enabled, std::sync::atomic::Ordering::Relaxed);
}

pub fn interrupted_instruction(sig: i32, pc: u64) -> Option<u32> {
    if sig != libc::SIGILL ||
        !CAPTURE_FAULT_INSTRUCTION.load(std::sync::atomic::Ordering::Relaxed) {
        return None;
    }
    let _errno = FaultReportErrno::save();
    read_fault_word(pc)
}

fn read_fault_word(pc: u64) -> Option<u32> {
    let mut word = 0u32;
    let mut got = 0u64;
    // SAFETY: copy four bytes from this task through the kernel; a bad PC
    // returns an error instead of faulting in the native signal handler.
    let result = unsafe {
        mach_vm_read_overwrite(mach_task_self_, pc, 4,
            &mut word as *mut u32 as u64, &mut got)
    };
    (result == 0 && got == 4).then_some(word)
}

struct FaultReportErrno(i32);
impl FaultReportErrno {
    fn save() -> Self {
        // SAFETY: Darwin's errno accessor belongs to the current native thread.
        Self(unsafe { *libc::__error() })
    }
}
impl Drop for FaultReportErrno {
    fn drop(&mut self) {
        // SAFETY: restore only this reporter's native-thread errno.
        unsafe { *libc::__error() = self.0; }
    }
}

fn fault_process_identity() {
    // Fatal reports can share a diagnostics stream with forked helpers.
    // Name the native process/thread before interpreting their addresses.
    crate::diag!("[linux-abi] host process {} pthread {:#x}",
        unsafe { libc::getpid() }, unsafe { libc::pthread_self() } as usize);
}

// A fatal fault can occur inside the Linux adapter. Its native registers then
// describe Rust/C locals, not the application's syscall arguments. Preserve the
// original live guest boundary alongside them; ordinary handled faults never
// enter this reporter and idle thread snapshots remain unattributed.
fn report_live_guest_syscall() {
    let context = crate::context::current_ctx();
    if context.is_null() { return; }
    let context = unsafe { &*context };
    if context.in_host == 0 { return; }
    crate::diag!("[linux-abi] live guest syscall {} tid {} pc {:#x} stub {:#x}",
        context.x[8], context.tid, context.pc, context.stub_ret);
    crate::diag!("[linux-abi]   guest arguments: x0 {:#x} x1 {:#x} x2 {:#x} x3 {:#x} x4 {:#x} x5 {:#x}",
        context.x[0], context.x[1], context.x[2], context.x[3], context.x[4], context.x[5]);
    crate::diag!("[linux-abi]   guest lr {:#x} ({}) sp {:#x} fp {:#x}",
        context.x[30], describe(context.x[30]), context.sp, context.x[29]);
    frames(context.x[29]);
}

// Bionic abort() raises a software SIGABRT, rather than taking a hardware
// fault. Its original default-signal owner already enters report_death(), but
// there is then no LAST_FAULT to describe. Use only the calling thread's live
// syscall context; an idle/stale guest snapshot must never become an abort
// attribution. The original bounded own-task reader remains the frame owner.
fn report_software_abort(sig: i32) {
    if sig != libc::SIGABRT { return; }
    let context = crate::context::current_ctx();
    if context.is_null() { return; }
    // SAFETY: current_ctx is this native thread's original live guest owner.
    let context = unsafe { &*context };
    if context.in_host == 0 { return; }
    let mut pc = context.pc;
    if context.stub_ret != 0 {
        // The original resume_pc() decodes this stub's final direct branch.
        // Read through the existing non-faulting kernel reader for diagnostics.
        if let Some(site) = context.stub_ret.checked_add(8) {
            if let Some(word) = read_fault_word(site) {
                if word & 0xfc00_0000 == 0x1400_0000 {
                    pc = site.wrapping_add(crate::a64::decode_b(word) as u64);
                }
            }
        }
    }
    fault_process_identity();
    crate::diag!("[linux-abi] fatal software SIGABRT at current guest syscall boundary: pc {pc:#x} ({}), syscall {} guest tid {}",
        describe(pc), context.x[8], context.tid);
    crate::diag!("[linux-abi]   lr {:#x} ({}) sp {:#x} fp {:#x}",
        context.x[30], describe(context.x[30]), context.sp, context.x[29]);
    let mut registers = [0; 29];
    registers.copy_from_slice(&context.x[..29]);
    fault_registers(&registers);
    fault_instructions(pc);
    frames(context.x[29]);
}

fn fault_registers(x: &[u64; 29]) {
    for i in (0..29).step_by(4) {
        let mut line = String::from("[linux-abi]  ");
        for j in i..(i + 4).min(29) {
            line.push_str(&format!(" x{j:<2} {:#018x}", x[j]));
        }
        crate::diag!("{line}");
    }
}

fn fault_instructions(pc: u64) {
    // This is a view at fatal-report time, after any guest signal handler.
    // The saved PC/registers above remain the original interrupted context.
    crate::diag!("[linux-abi]   instruction words at fatal report (saved pc {pc:#x}):");
    let center = pc & !3;
    for index in -4i32..=4 {
        let address = if index < 0 {
            center.checked_sub((-index) as u64 * 4)
        } else {
            center.checked_add(index as u64 * 4)
        };
        let Some(address) = address else { continue; };
        let mut word = 0u32;
        let mut got = 0u64;
        // SAFETY: the same original own-task kernel reader used by frames().
        // Each word is independent, including across unreadable page edges;
        // no guest pointer is dereferenced and no guest linker is entered.
        let result = unsafe {
            mach_vm_read_overwrite(mach_task_self_, address, 4,
                &mut word as *mut u32 as u64, &mut got)
        };
        let marker = if index == 0 { " pc" } else { "" };
        if result == 0 && got == 4 {
            crate::diag!("[linux-abi]   insn {address:#018x}: {word:08x}{marker}");
        } else {
            crate::diag!("[linux-abi]   insn {address:#018x}: unreadable kr={result} bytes={got}{marker}");
        }
    }
}
