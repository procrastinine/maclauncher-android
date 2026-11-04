// Linux sync_file.h, shared with the original syscall owner's FileInfo ABI.
#[repr(C)]
#[derive(Default)]
struct NativeFenceInfo {
    name: [u8; 32],
    status: i32,
    flags: u32,
    num_fences: u32,
    pad: u32,
    sync_fence_info: u64,
}
const _: () = assert!(size_of::<NativeFenceInfo>() == 56);
const SYNC_IOC_FILE_INFO: libc::c_ulong = 0xc038_3e04;

/// Readiness retires a fence, but only its original status establishes success.
/// Inspect without consuming its record or changing the borrowed descriptor.
fn poll(fd: i32, timeout: u64) -> Result<bool, EGLint> {
    let ms = if timeout == FOREVER { -1 } else {
        timeout.div_ceil(1_000_000).min(i32::MAX as u64) as i32
    };
    let mut descriptor = libc::pollfd { fd, events: libc::POLLIN, revents: 0 };
    loop {
        let count = unsafe { libc::poll(&mut descriptor, 1, ms) };
        if count == 0 { return Ok(false); }
        if count < 0 {
            if std::io::Error::last_os_error().raw_os_error() == Some(libc::EINTR) { continue; }
            return Err(EGL_BAD_ALLOC);
        }
        let mut info = NativeFenceInfo::default();
        loop {
            if unsafe { libc::ioctl(fd, SYNC_IOC_FILE_INFO as _, &mut info as *mut NativeFenceInfo) } == 0 { break; }
            if std::io::Error::last_os_error().raw_os_error() != Some(libc::EINTR) {
                return Err(EGL_BAD_ALLOC);
            }
        }
        return if info.status < 0 { Err(EGL_BAD_ALLOC) } else { Ok(info.status > 0) };
    }
}
