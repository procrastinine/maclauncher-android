// Included inside the original attrs owner, reusing its inode lock, codec and
// ctime cache. Only imported files without complete guest metadata reach here.
fn imported_path(host: Host) -> Option<std::path::PathBuf> {
    use std::os::unix::ffi::OsStrExt;
    match host {
        Host::Image(path) | Host::Path(path) => Some(std::path::PathBuf::from(std::ffi::OsStr::from_bytes(path.to_bytes()))),
        Host::Fd(fd) => {
            let mut bytes = [0u8; libc::PATH_MAX as usize];
            if unsafe { libc::fcntl(fd, libc::F_GETPATH, bytes.as_mut_ptr()) } < 0 { return None; }
            let size = bytes.iter().position(|b| *b == 0)?;
            Some(std::path::PathBuf::from(std::ffi::OsStr::from_bytes(&bytes[..size])))
        }
    }
}

fn imported_attrs(host: Host, st: &libc::stat, moving: bool) -> Result<Option<Attr>, crate::errno::Errno> {
    let Some(path) = imported_path(host) else { return Ok(None); };
    let Some(root) = vfs::imported_root(&path) else { return Ok(None); };
    let map_error = |error: std::io::Error| crate::errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO));
    let Some(owner) = aim_storage::imported_inodes::defaults(&path, &root).map_err(map_error)? else { return Ok(None); };
    if aim_storage::imported_inodes::code_owner(owner) {
        // An enrolled read-only tree has a declared Linux identity. Read any
        // existing explicit fields, but never materialize defaults through a
        // write to its immutable host inode (including descriptor queries).
        let mut bytes = [0u8; 20];
        let old = get(host, GUEST, &mut bytes).and_then(|length| decode(&bytes[..length])).unwrap_or_default();
        let mode = match st.st_mode & libc::S_IFMT {
            libc::S_IFDIR => 0o755,
            libc::S_IFLNK => 0o777,
            _ => aim_storage::imported_inodes::code_mode(st.st_mode as u32),
        };
        return Ok(Some(old.over(Attr { uid: owner.uid, gid: owner.gid, mode: Some(mode) })));
    }
    with_inode_lock(host, st, || {
        if moving && st.st_mode & libc::S_IFMT == libc::S_IFDIR {
            aim_storage::imported_inodes::preserve_tree(&path, owner).map_err(map_error)?;
        }
        let old = guest_attr(host).unwrap_or_default();
        let value = old.over(Attr { uid: owner.uid, gid: owner.gid, mode: Some(st.st_mode as u32 & 0o7777) });
        if value != old && !set_guest(host, value)? { return Err(crate::errno::EIO); }
        Ok(Some(value))
    })
}

/// Materialize an import before a new link or directory move changes its
/// ancestry. Untouched descendants inherit the default carried by the directory.
pub(super) fn preserve_import(host: Host) -> Result<(), crate::errno::Errno> {
    if !recording() { return Ok(()); }
    let st = host_stat(host)?;
    imported_attrs(host, &st, true).map(|_| ())
}
