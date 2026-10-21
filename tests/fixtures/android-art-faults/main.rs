use aim_linux_abi::diag::{self, Fault};
use std::sync::atomic::{AtomicBool, AtomicU8, Ordering::Relaxed};

static MODE: AtomicU8 = AtomicU8::new(0);
static HANDLED: AtomicBool = AtomicBool::new(false);
const ERRNO: i32 = 173;

unsafe fn errno() -> i32 { unsafe { *libc::__error() } }
unsafe fn set_errno(value: i32) { unsafe { *libc::__error() = value; } }

unsafe fn line(value: &[u8]) {
    unsafe { libc::write(1, value.as_ptr().cast(), value.len()); }
}

unsafe extern "C" fn handler(sig: i32, info: *mut libc::siginfo_t, context: *mut libc::c_void) {
    unsafe {
        if MODE.load(Relaxed) == 2 || MODE.load(Relaxed) == 3 {
            if sig != libc::SIGBUS && sig != libc::SIGSEGV { libc::_exit(87); }
            let uc = &mut *(context as *mut libc::ucontext_t);
            let mc = &mut *uc.uc_mcontext;
            let ss = &mut mc.__ss;
            if ss.__x[0] != 0x1122 || ss.__x[5] != 0x5566 || ss.__x[16] != 0x7788 { libc::_exit(88); }
            let snapshot = Fault {
                sig: libc::SIGSEGV, pc: ss.__pc, lr: ss.__lr, fp: ss.__fp, sp: ss.__sp,
                addr: (*info).si_addr as u64, esr: mc.__es.__esr, x: ss.__x, instruction: None,
            };
            set_errno(ERRNO);
            diag::note_fault(snapshot);
            // The original Linux sigframe owner translates a Darwin protected
            // page fault to Linux SIGSEGV/SEGV_ACCERR before this boundary.
            diag::report_handled_fault(&snapshot, 2, 719);
            if errno() != ERRNO { libc::_exit(89); }
            HANDLED.store(true, Relaxed); ss.__pc += 4;
            return;
        }
        if sig != libc::SIGILL { libc::_exit(81); }
        if MODE.load(Relaxed) == 1 && HANDLED.swap(true, Relaxed) {
            set_errno(ERRNO);
            diag::report_death(sig);
            if errno() != ERRNO { libc::_exit(82); }
            line(b"PASS saved hardware fault re-raise preserved errno\n");
            libc::_exit(0);
        }
        // The first invocation comes from an actual AArch64 UDF instruction,
        // with a kernel-provided machine context, rather than raise().
        let uc = &mut *(context as *mut libc::ucontext_t);
        let mc = &mut *uc.uc_mcontext;
        let ss = &mut mc.__ss;
        if ss.__pc & 3 != 0 || (ss.__pc as *const u32).read() != 0 { libc::_exit(83); }
        if ss.__x[0] != 0x1122 || ss.__x[5] != 0x5566 || ss.__x[16] != 0x7788 { libc::_exit(84); }
        diag::write_line(format_args!("[fault-probe] actual Darwin SIGILL si_code {}", (*info).si_code));
        set_errno(ERRNO);
        if MODE.load(Relaxed) == 0 || MODE.load(Relaxed) == 4 {
            diag::report(sig, info, context);
            if errno() != ERRNO { libc::_exit(85); }
            line(b"PASS actual hardware SIGILL report preserved errno\n");
            libc::_exit(0);
        }
        diag::note_fault(Fault {
            sig, pc: ss.__pc, lr: ss.__lr, fp: ss.__fp, sp: ss.__sp,
            addr: (*info).si_addr as u64, esr: mc.__es.__esr, x: ss.__x,
            instruction: diag::interrupted_instruction(sig, ss.__pc),
        });
        if errno() != ERRNO { libc::_exit(86); }
        // A guest handler can resume from an intentional synchronous fault.
        // Preserve that original snapshot while this fixture skips the UDF.
        ss.__pc += 4;
    }
}

#[inline(never)]
unsafe fn protected_store(page: *mut libc::c_void) {
    unsafe {
        std::arch::asm!(
            "mov x0, #0x1122", "mov x5, #0x5566", "mov x16, #0x7788",
            ".inst 0xf9000020", // str x0,[x1], into the read-only page
            in("x1") page, out("x0") _, out("x5") _, out("x16") _,
            options(nostack),
        );
    }
}

#[inline(never)]
unsafe fn hardware_fault() {
    unsafe {
        std::arch::asm!(
            "mov x0, #0x1122", "mov x5, #0x5566", "mov x16, #0x7788", "nop",
            ".inst 0x00000000", "nop", "nop", "nop", "nop",
            out("x0") _, out("x5") _, out("x16") _,
            options(nostack),
        );
    }
}

fn install() {
    unsafe {
        let mut action: libc::sigaction = std::mem::zeroed();
        action.sa_sigaction = handler as *const () as usize;
        action.sa_flags = libc::SA_SIGINFO;
        libc::sigemptyset(&mut action.sa_mask);
        let sig = match MODE.load(Relaxed) { 2 | 3 => libc::SIGBUS, _ => libc::SIGILL };
        assert_eq!(libc::sigaction(sig, &action, std::ptr::null_mut()), 0);
    }
}

fn handled_protection(enabled: bool) {
    MODE.store(2, Relaxed); install();
    diag::configure_handled_fault_capture(enabled);
    unsafe {
        let page = libc::mmap(std::ptr::null_mut(), 16384, libc::PROT_READ,
            libc::MAP_PRIVATE | libc::MAP_ANON, -1, 0);
        assert_ne!(page, libc::MAP_FAILED);
        set_errno(ERRNO); protected_store(page);
        assert_eq!(errno(), ERRNO); assert!(HANDLED.load(Relaxed));
        assert_eq!(libc::munmap(page, 16384), 0);
        line(b"PASS actual handled protection fault resumed with original errno and no fatal report\n");
    }
}

fn handled_bounds() {
    let mut fault = saved(0x10000); fault.sig = libc::SIGSEGV;
    unsafe {
        diag::configure_handled_fault_capture(false);
        set_errno(ERRNO); diag::report_handled_fault(&fault, 2, 719); assert_eq!(errno(), ERRNO);
        diag::configure_handled_fault_capture(true);
        diag::report_handled_fault(&fault, 1, 719); // ordinary implicit-null fault
        fault.sig = libc::SIGILL; diag::report_handled_fault(&fault, 2, 719);
        fault.sig = libc::SIGSEGV;
        for _ in 0..20 { diag::report_handled_fault(&fault, 2, 719); assert_eq!(errno(), ERRNO); }
        // A retired diagnostics socket cannot inject a new SIGPIPE or wait for
        // a reader while the guest's original signal handler is pending.
        let mut sockets = [0; 2];
        assert_eq!(libc::socketpair(libc::AF_UNIX, libc::SOCK_STREAM, 0, sockets.as_mut_ptr()), 0);
        assert_eq!(libc::close(sockets[1]), 0); diag::log_to(sockets[0]);
        diag::configure_handled_fault_capture(true); set_errno(ERRNO);
        diag::report_handled_fault(&fault, 2, 719); assert_eq!(errno(), ERRNO);
        diag::log_to(2); assert_eq!(libc::close(sockets[0]), 0);
        line(b"PASS handled context is opt-in, protection-only, bounded and safe after diagnostic retirement\n");
    }
}

fn saved(pc: u64) -> Fault {
    let mut x = [0; 29]; x[0] = 0xaabb; x[28] = 0xccdd;
    Fault { sig: libc::SIGILL, pc, lr: 0, fp: 1, sp: 0x1234, addr: 0x5678, esr: 0x9abc, x, instruction: None }
}

fn records() {
    unsafe {
        let page = libc::mmap(std::ptr::null_mut(), 4096, libc::PROT_READ | libc::PROT_WRITE,
            libc::MAP_PRIVATE | libc::MAP_ANON, -1, 0);
        assert_ne!(page, libc::MAP_FAILED);
        let words = page as *mut u32;
        for index in 0..9 { words.add(index).write(0xd503201f); }
        words.add(4).write(0x11111111);
        set_errno(ERRNO);
        assert_eq!(diag::interrupted_instruction(libc::SIGILL, words.add(4) as u64), None);
        diag::configure_fault_instruction_capture(true);
        assert_eq!(diag::interrupted_instruction(libc::SIGSEGV, words.add(4) as u64), None);
        let mut original = saved(words.add(4) as u64);
        original.instruction = diag::interrupted_instruction(original.sig, original.pc);
        assert_eq!(original.instruction, Some(0x11111111));
        diag::note_fault(original); assert_eq!(errno(), ERRNO);
        // The fatal report must read the current word, while retaining the
        // original register snapshot and the explicitly captured SIGILL word.
        words.add(4).write(0x22222222);
        set_errno(ERRNO); diag::report_death(libc::SIGILL); assert_eq!(errno(), ERRNO);
        diag::report_death(libc::SIGILL); assert_eq!(errno(), ERRNO);
        assert_eq!(libc::munmap(page, 4096), 0);
        assert_eq!(diag::interrupted_instruction(libc::SIGILL, 0x10000), None);
        set_errno(ERRNO); diag::note_fault(saved(0x10000)); assert_eq!(errno(), ERRNO);
        diag::report_death(libc::SIGILL); assert_eq!(errno(), ERRNO);
        // A different terminating signal consumes the stale snapshot without
        // attributing it to either the mismatched signal or a later death.
        diag::note_fault(saved(0x20000));
        diag::report_death(libc::SIGSEGV); assert_eq!(errno(), ERRNO);
        diag::report_death(libc::SIGILL); assert_eq!(errno(), ERRNO);
        line(b"PASS report-time instruction words, unreadable PC, once-only and mismatched signal handling preserve errno\n");
    }
}

fn software_abort() {
    unsafe {
        // A plain host abort and an idle guest snapshot have no live guest
        // syscall to attribute. Neither may acquire an invented fault report.
        set_errno(ERRNO); diag::report_death(libc::SIGABRT); assert_eq!(errno(), ERRNO);
        let context = &mut *aim_linux_abi::context::init_thread();
        let code = [0xd503201fu32; 9];
        let mut stub = [0xd503201fu32; 3];
        let frames = [0u64, code.as_ptr() as u64];
        let pc = code.as_ptr().add(4) as u64;
        let branch = stub.as_ptr().add(2) as u64;
        let distance = pc as i64 - branch as i64;
        assert_eq!(distance & 3, 0);
        assert!(distance >= -(1 << 27) && distance < (1 << 27));
        stub[2] = 0x14000000 | ((distance / 4) as u32 & 0x03ffffff);
        context.pc = 0x10000; // original dispatch stub is authoritative
        context.stub_ret = stub.as_ptr() as u64;
        context.x[0] = 0xaabb;
        context.x[8] = 131; // original tgkill syscall
        context.x[28] = 0xccdd;
        context.x[29] = frames.as_ptr() as u64;
        context.x[30] = code.as_ptr() as u64;
        context.tid = 719;
        context.in_host = 0;
        diag::report_death(libc::SIGABRT); assert_eq!(errno(), ERRNO);
        context.in_host = 1;
        diag::report_death(libc::SIGTERM); assert_eq!(errno(), ERRNO);
        diag::report_death(libc::SIGABRT); assert_eq!(errno(), ERRNO);
        line(b"PASS software abort uses only the current live guest syscall context and preserves errno\n");
    }
}

fn abort_request() {
    #[repr(align(16))]
    struct Frames([u64; 2]);
    unsafe {
        set_errno(ERRNO);
        diag::configure_abort_request_capture(true);
        diag::report_abort_request(libc::SIGABRT, 719); // no current guest
        let context = &mut *aim_linux_abi::context::init_thread();
        let frames = Frames([0xfffffffffffffff0u64, 0x12345678]); // unreadable next frame
        context.pc = 0x11223344;
        context.stub_ret = 0x55667788;
        context.x[8] = 131;
        context.x[29] = frames.0.as_ptr() as u64;
        context.x[30] = 0xaabbccdd;
        context.tid = 719;
        context.in_host = 0;
        diag::report_abort_request(libc::SIGABRT, 719);
        context.in_host = 1;
        diag::configure_abort_request_capture(false);
        diag::report_abort_request(libc::SIGABRT, 719);
        diag::configure_abort_request_capture(true);
        diag::report_abort_request(libc::SIGTERM, 719);
        diag::report_abort_request(libc::SIGABRT, 720);
        context.x[8] = 135;
        diag::report_abort_request(libc::SIGABRT, 719);
        context.x[8] = 131;
        for _ in 0..20 {
            diag::report_abort_request(libc::SIGABRT, 719);
            assert_eq!(errno(), ERRNO);
        }
        let mut sockets = [0; 2];
        assert_eq!(libc::socketpair(libc::AF_UNIX, libc::SOCK_STREAM, 0, sockets.as_mut_ptr()), 0);
        assert_eq!(libc::close(sockets[1]), 0);
        diag::log_to(sockets[0]); diag::configure_abort_request_capture(true);
        set_errno(ERRNO); diag::report_abort_request(libc::SIGABRT, 719);
        assert_eq!(errno(), ERRNO);
        diag::log_to(2); assert_eq!(libc::close(sockets[0]), 0);
        line(b"PASS abort request capture is opt-in, live self-only, bounded and safe after diagnostic retirement\n");
    }
}

fn main() {
    match std::env::args().nth(1).as_deref() {
        Some("direct") => { install(); unsafe { hardware_fault(); } panic!("hardware fault unexpectedly returned"); }
        Some("handled") => {
            diag::configure_fault_instruction_capture(true);
            MODE.store(1, Relaxed); install();
            unsafe {
                set_errno(ERRNO); hardware_fault(); assert_eq!(errno(), ERRNO);
                assert!(HANDLED.load(Relaxed));
                // Re-raise only after the original hardware context resumed.
                libc::raise(libc::SIGILL);
            }
            panic!("re-raised signal unexpectedly returned");
        }
        Some("records") => records(),
        Some("abort") => software_abort(),
        Some("abort-request") => abort_request(),
        Some("host-syscall") => {
            let context = unsafe { &mut *aim_linux_abi::context::init_thread() };
            context.in_host=1; context.tid=719; context.x[8]=135;
            context.x[0]=2; context.x[1]=0x6359c40; context.x[3]=8;
            MODE.store(4,Relaxed); install(); unsafe { hardware_fault(); }
        }
        Some("segv-off") => handled_protection(false),
        Some("segv-on") => handled_protection(true),
        Some("segv-bounds") => handled_bounds(),
        _ => panic!("expected direct, handled, records or segv mode"),
    }
}
