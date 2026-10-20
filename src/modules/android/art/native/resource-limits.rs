// Linux NOFILE is bounded by the kernel's actual descriptor-table ceiling,
// including when Darwin inherited RLIM_INFINITY or a larger soft rlimit.
// Other resources and lower explicitly selected limits retain their values.
fn descriptor_ceiling() -> Result<u64, i64> {
    let mut value: libc::c_int = 0;
    let mut size = std::mem::size_of_val(&value);
    let status = unsafe {
        libc::sysctlbyname(c"kern.maxfilesperproc".as_ptr(),
            (&mut value as *mut libc::c_int).cast(), &mut size,
            std::ptr::null_mut(), 0)
    };
    if status != 0 { return Err(-(crate::errno::last() as i64)); }
    if size != std::mem::size_of_val(&value) || value <= 0 {
        return Err(-(crate::errno::EIO as i64));
    }
    Ok(value as u64)
}

pub(crate) fn current(resource: u64, limits: (u64, u64)) -> Result<(u64, u64), i64> {
    if resource != 7 { return Ok(limits); }
    let maximum = limits.1.min(descriptor_ceiling()?);
    Ok((limits.0.min(maximum), maximum))
}

pub(crate) fn request(resource: u64, maximum: u64) -> Result<(), i64> {
    if resource == 7 && maximum > descriptor_ceiling()? {
        return Err(-(crate::errno::EPERM as i64));
    }
    Ok(())
}
