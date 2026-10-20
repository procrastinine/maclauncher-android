//! Defaults for host-imported application trees. Explicit guest inode records
//! remain authoritative; newly created guest files use the original create path.
use crate::guest_inode::{self, GuestInode};
use std::{ffi::CString, io, os::unix::{ffi::OsStrExt, fs::PermissionsExt}, path::Path};

const NAME: &std::ffi::CStr = c"dev.aim.import-owner";
const CODE_FILE_MODE: u32 = 0o644;

/// Host-private permissions are not Android's installed-code ownership.
/// Preserve whether a code file is executable, while projecting that bit for
/// the same guest readers as its root-owned read-only contents.
pub fn code_mode(host_mode: u32) -> u32 {
    CODE_FILE_MODE | if host_mode & 0o111 != 0 { 0o111 } else { 0 }
}

fn read(path: &Path) -> io::Result<Option<GuestInode>> {
    let path = CString::new(path.as_os_str().as_bytes()).map_err(io::Error::other)?;
    let mut bytes = [0u8; 20];
    let size = unsafe { libc::getxattr(path.as_ptr(), NAME.as_ptr(), bytes.as_mut_ptr().cast(), bytes.len(), 0, libc::XATTR_NOFOLLOW) };
    if size < 0 {
        let error = io::Error::last_os_error();
        return if matches!(error.raw_os_error(), Some(libc::ENOATTR | libc::ENOENT | libc::ENOTDIR)) { Ok(None) } else { Err(error) };
    }
    Ok(guest_inode::decode(&bytes[..size as usize]).filter(|a| a.uid.is_some() && a.gid.is_some()))
}

/// Keep the import default on the actual directory inode. It follows directory
/// renames without changing the explicit owner of that directory or its children.
pub fn preserve_tree(path: &Path, owner: GuestInode) -> io::Result<()> {
    if read(path)?.is_some() { return Ok(()); }
    let bytes = guest_inode::encode(GuestInode { mode: None, ..owner });
    match guest_inode::write_attribute(path, &bytes, NAME.to_bytes_with_nul(), libc::XATTR_CREATE) {
        Err(error) if error.raw_os_error() == Some(libc::EEXIST) => Ok(()),
        result => result,
    }
}

/// The existing import declaration also carries immutable installed-code
/// defaults. A present mode is its file policy; mutable data keeps mode absent.
pub fn code_owner(owner: GuestInode) -> bool {
    owner.uid == Some(0) && owner.gid == Some(0) && owner.mode == Some(CODE_FILE_MODE)
}

pub fn code_tree(path: &Path) -> io::Result<bool> {
    Ok(read(path)?.is_some_and(code_owner))
}

fn preserve_code(path: &Path) -> io::Result<()> {
    let owner = GuestInode { uid: Some(0), gid: Some(0), mode: Some(CODE_FILE_MODE) };
    if let Some(old) = read(path)? {
        return if code_owner(old) { Ok(()) } else { Err(io::Error::other("Android code import conflicts with existing storage ownership")) };
    }
    match guest_inode::write_attribute(path, &guest_inode::encode(owner), NAME.to_bytes_with_nul(), libc::XATTR_CREATE) {
        Err(error) if error.raw_os_error() == Some(libc::EEXIST) => {
            if code_tree(path)? { Ok(()) } else { Err(io::Error::other("Android code import conflicts with existing storage ownership")) }
        }
        result => result,
    }
}

/// Declare one enrolled code root without visiting its descendants. Existing
/// explicit guest inode fields remain authoritative, including on later starts.
pub fn initialize_code(path: &Path) -> io::Result<()> {
    let metadata = std::fs::symlink_metadata(path)?;
    if metadata.file_type().is_symlink() { return Ok(()); }
    if metadata.is_dir() { return preserve_code(path); }
    let old = guest_inode::read(path)?.unwrap_or_default();
    let value = GuestInode {
        uid: old.uid.or(Some(0)), gid: old.gid.or(Some(0)),
        mode: old.mode.or(Some(code_mode(metadata.permissions().mode()))),
    };
    if old != value { guest_inode::record(path, value)?; }
    preserve_code(path)
}

/// A declaration, not an initialization-complete stamp. Host-created nested
/// files added later receive this same default when the guest first uses them.
pub fn initialize(path: &Path, uid: u32) -> io::Result<()> {
    let metadata = std::fs::symlink_metadata(path)?;
    if !metadata.is_dir() { return Err(io::Error::new(io::ErrorKind::InvalidInput, "Imported Android storage must be a directory")); }
    let old = guest_inode::read(path)?.unwrap_or_default();
    let value = GuestInode { uid: old.uid.or(Some(uid)), gid: old.gid.or(Some(uid)), mode: old.mode.or(Some(metadata.permissions().mode() & 0o7777)) };
    if old != value { guest_inode::record(path, value)?; }
    preserve_tree(path, GuestInode { uid: Some(uid), gid: Some(uid), mode: None })
}

/// Paths here are the VFS's resolved physical inode paths, never guest paths.
/// Stop at the existing declared mount owner. No host ancestors, directory
/// enumeration or host UID/GID fallback are involved.
pub fn defaults(path: &Path, root: &Path) -> io::Result<Option<GuestInode>> {
    for ancestor in path.ancestors().take_while(|path| path.starts_with(root)) {
        if let Some(owner) = read(ancestor)? { return Ok(Some(owner)); }
    }
    Ok(None)
}
