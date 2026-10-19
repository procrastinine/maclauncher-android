// Uses the original syscall trace switch and diagnostics descriptor. Only
// already-rejected publication reaches this path; no guest memory is read.
static PUBLICATION_REJECTIONS: std::sync::atomic::AtomicUsize =
    std::sync::atomic::AtomicUsize::new(0);

fn trace_publication_rejection(stage: &str, site: u64, lo: u64, hi: u64,
                               original: u32, placed: bool, errno: i32) {
    use std::sync::atomic::Ordering::Relaxed;
    if !crate::sys::tracing() || PUBLICATION_REJECTIONS.fetch_add(1, Relaxed) >= 8 {
        return;
    }
    let saved_errno = unsafe { *libc::__error() };
    let reason = if placed { "veneer-emission" } else { "island-placement" };
    crate::diag!("[linux-abi] executable publication rejected stage={stage} reason={reason} pid={} tid={} site={site:#x} offset={:#x} range={lo:#x}..{hi:#x} original={original:08x} errno={errno}",
        unsafe { libc::getpid() }, crate::sys::host_tid(), site.wrapping_sub(lo));
    unsafe { *libc::__error() = saved_errno; }
}
