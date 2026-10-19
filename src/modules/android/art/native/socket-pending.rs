use crate::errno::{self, Errno};
use super::super::fdtab::{Kind, Pinned};
use super::{Family, SockType};
use std::os::fd::AsRawFd;

/// Linux UDP FIONREAD reports the first datagram's payload, not Darwin's
/// aggregate receive storage (which includes source addresses and control).
pub(in crate::sys) fn pending_bytes(input: &Pinned) -> Option<Result<i32, Errno>> {
    let Some(Kind::Sock(socket)) = input.kind() else { return None; };
    let Family::Inet(options) = &socket.family else { return None; };
    if socket.ty != SockType::Dgram || options.icmp4.load(std::sync::atomic::Ordering::Relaxed) {
        return None;
    }
    let mut count = 0i32;
    let mut length = std::mem::size_of_val(&count) as libc::socklen_t;
    // SO_NREAD inspects the first record under the kernel socket lock. It
    // neither consumes a packet nor copies its payload into this process.
    let result = unsafe {
        libc::getsockopt(input.descriptor().as_raw_fd(), libc::SOL_SOCKET, libc::SO_NREAD,
            (&mut count as *mut i32).cast(), &mut length)
    };
    Some(if result < 0 { Err(errno::last()) } else { Ok(count) })
}
