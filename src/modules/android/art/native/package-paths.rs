//! Package aliases retain the original read-only inode and mount provenance.
use crate::{errno, vfs};
use std::{os::unix::ffi::OsStrExt, path::PathBuf};

pub(crate) fn bind(address: u64, length: u64) -> i64 {
    match publish(address, length) { Ok(()) => 0, Err(error) => -(error as i64) }
}
fn publish(address: u64, length: u64) -> Result<(), errno::Errno> {
    if !(10..=8200).contains(&length) { return Err(errno::EINVAL); }
    let packet = super::user_memory::read_exact(address, length as usize)?;
    let alias_len = u32::from_le_bytes(packet[..4].try_into().unwrap()) as usize;
    let source_len = u32::from_le_bytes(packet[4..8].try_into().unwrap()) as usize;
    if alias_len == 0 || source_len == 0 || alias_len >= 4096 || source_len >= 4096 || 8 + alias_len + source_len != packet.len() { return Err(errno::EINVAL); }
    let alias = &packet[8..8 + alias_len];
    let source = &packet[8 + alias_len..];
    if alias[0] != b'/' || source[0] != b'/' || alias.contains(&0) || source.contains(&0) { return Err(errno::EINVAL); }
    let identity = super::cred::current();
    let source = vfs::resolve_checked(vfs::LINUX_AT_FDCWD, source, true, |path| super::attrs::search(path, &identity, 3))?;
    let stat = super::attrs::path_stat(&source.guest)?;
    if source.area != vfs::Area::Image || stat.st_mode & libc::S_IFMT != libc::S_IFREG { return Err(errno::EINVAL); }
    if !super::attrs::permits(&stat, 4, &identity, 3) { return Err(errno::EACCES); }
    let alias = vfs::resolve_checked(vfs::LINUX_AT_FDCWD, alias, false, |path| super::attrs::search(path, &identity, 3))?;
    match super::attrs::path_stat(&alias.guest) {
        Ok(_) => return Err(errno::EEXIST),
        Err(errno::ENOENT) => {},
        Err(error) => return Err(error),
    }
    let host = PathBuf::from(std::ffi::OsStr::from_bytes(source.host.to_bytes()));
    vfs::bind_mount(&source.guest, &alias.guest, host, source.area, &source.guest, "bind")
}
