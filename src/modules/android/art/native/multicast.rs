//! Linux multicast option records on the existing Darwin socket. The option
//! numbers match the shared JVM native socket boundary; interface indices use
//! the original Linux device owner rather than exposing host interface numbers.
use crate::errno::{self, Errno, EINVAL, ENODEV};
use super::super::{netif, uplink, user_memory};
use super::{SocketInput, Family, any_sock};
use std::sync::atomic::Ordering;

const V4: u64 = 0;
const V6: u64 = 41;
const EADDRNOTAVAIL: Errno = 99;
const ENOPROTOOPT: Errno = 92;
const EPROTO: Errno = 71;

fn read(address: u64, length: usize) -> Result<Vec<u8>, Errno> {
    user_memory::read_exact(address, length)
}
fn int(bytes: &[u8]) -> i32 { i32::from_ne_bytes(bytes[..4].try_into().unwrap()) }
fn native_set(fd: i32, level: i32, option: i32, bytes: &[u8]) -> Result<(), Errno> {
    // SAFETY: a private initialized option record, retained for the call.
    let result = unsafe { libc::setsockopt(fd, level, option, bytes.as_ptr().cast(), bytes.len() as u32) };
    if result < 0 { Err(errno::last()) } else { Ok(()) }
}
fn native_get(fd: i32, level: i32, option: i32) -> Result<[u8; 4], Errno> {
    let mut value = [0u8; 4];
    let mut length = 4;
    // SAFETY: private output record with its exact capacity.
    let result = unsafe { libc::getsockopt(fd, level, option, value.as_mut_ptr().cast(), &mut length) };
    if result < 0 { Err(errno::last()) } else { Ok(value) }
}
fn stream(fd: i32) -> Result<bool, Errno> {
    Ok(int(&native_get(fd, libc::SOL_SOCKET, libc::SO_TYPE)?) == libc::SOCK_STREAM)
}
fn host_index(index: u32) -> Result<u32, Errno> {
    if index == 0 { return Ok(0); }
    let link = netif::by_index(index).ok_or(ENODEV)?;
    let host = netif::host_index_of(&link.name)?;
    if host == 0 { Err(ENODEV) } else { Ok(host) }
}
fn guest_index(index: u32) -> Result<u32, Errno> {
    if index == 0 { return Ok(0); }
    for link in netif::links() {
        if netif::host_index_of(&link.name)? == index { return Ok(link.index); }
    }
    Err(ENODEV)
}
fn ipv4_request(address: u64, length: u32, interface: bool) -> Result<[u8; 12], Errno> {
    let count = if length >= 12 { 12 } else if length >= 8 { 8 }
        else if interface && length >= 4 { 4 } else { return Err(EINVAL); };
    let bytes = read(address, count)?;
    let mut request = [0u8; 12];
    if count == 4 { request[4..8].copy_from_slice(&bytes); }
    else { request[..count].copy_from_slice(&bytes); }
    let index = int(&request[8..]) as u32;
    if index != 0 {
        let host = host_index(index).map_err(|error| if interface && error == ENODEV { EADDRNOTAVAIL } else { error })?;
        request[8..12].copy_from_slice(&host.to_ne_bytes());
    }
    Ok(request)
}

fn set_value(input: &SocketInput, level: u64, option: u64, address: u64, length: u32) -> Result<(), Errno> {
    let fd = input.fd();
    match (level, option) {
        (V4, 33 | 34) => {
            if length == 0 { return Err(EINVAL); }
            let bytes = read(address, if length >= 4 { 4 } else { 1 })?;
            let mut value = if length >= 4 { int(&bytes) } else { bytes[0] as i32 };
            let host = if option == 33 {
                if stream(fd)? { return Err(EINVAL); }
                if value == -1 { value = 1; }
                if !(0..=255).contains(&value) { return Err(EINVAL); }
                libc::IP_MULTICAST_TTL
            } else {
                value = (value != 0) as i32;
                libc::IP_MULTICAST_LOOP
            };
            // Darwin's IPv4 multicast scalar record is an unsigned byte.
            native_set(fd, libc::IPPROTO_IP, host, &[value as u8])
        }
        (V4, 32) => {
            if stream(fd)? { return Err(EINVAL); }
            let request = ipv4_request(address, length, true)?;
            if int(&request[8..]) == 0 {
                native_set(fd, libc::IPPROTO_IP, libc::IP_MULTICAST_IF, &request[4..8])?;
            } else {
                let mut host = request;
                host[4..8].fill(0);
                native_set(fd, libc::IPPROTO_IP, libc::IP_MULTICAST_IF, &host)?;
            }
            if let Some(socket) = any_sock(input) {
                if let Family::Inet(owner) = &socket.family {
                    owner.multicast_v4_address.store(u32::from_ne_bytes(request[4..8].try_into().unwrap()), Ordering::Relaxed);
                }
            }
            Ok(())
        }
        (V4, 35 | 36) => {
            if stream(fd)? { return Err(EPROTO); }
            let mut request = ipv4_request(address, length, false)?;
            let host = if option == 35 { libc::IP_ADD_MEMBERSHIP } else { libc::IP_DROP_MEMBERSHIP };
            if int(&request[8..]) == 0 {
                native_set(fd, libc::IPPROTO_IP, host, &request[..8])
            } else {
                request[4..8].fill(0);
                native_set(fd, libc::IPPROTO_IP, host, &request)
            }
        }
        (V6, 17 | 18 | 19) => {
            if length < 4 { return Err(EINVAL); }
            let mut value = int(&read(address, 4)?);
            let default_interface = option == 17 && value == 0;
            let host = match option {
                17 => {
                    if stream(fd)? { return Err(ENOPROTOOPT); }
                    if default_interface {
                        let current = int(&native_get(fd, libc::IPPROTO_IPV6, libc::IPV6_MULTICAST_IF)?);
                        // Darwin cannot set index zero. A new socket already
                        // has its default selection; resetting an explicit
                        // selection follows the current physical route.
                        value = if current == 0 { 0 } else { uplink::current().map_or(current, |route| route.index as i32) };
                    } else { value = host_index(value as u32)? as i32; }
                    libc::IPV6_MULTICAST_IF
                }
                18 => {
                    if stream(fd)? { return Err(ENOPROTOOPT); }
                    if !(-1..=255).contains(&value) { return Err(EINVAL); }
                    if value == -1 { value = 1; }
                    libc::IPV6_MULTICAST_HOPS
                }
                _ => {
                    if value != 0 && value != 1 { return Err(EINVAL); }
                    libc::IPV6_MULTICAST_LOOP
                }
            };
            if !(default_interface && value == 0) {
                native_set(fd, libc::IPPROTO_IPV6, host, &value.to_ne_bytes())?;
            }
            if option == 17 {
                if let Some(socket) = any_sock(input) {
                    if let Family::Inet(owner) = &socket.family {
                        owner.multicast_v6_default.store(default_interface, Ordering::Relaxed);
                    }
                }
            }
            Ok(())
        }
        (V6, 20 | 21) => {
            if length < 20 { return Err(EINVAL); }
            if stream(fd)? { return Err(EPROTO); }
            let mut request = read(address, 20)?;
            let index = int(&request[16..]) as u32;
            let host = if index != 0 { host_index(index)? } else {
                // Linux chooses a multicast route for interface zero. Reuse
                // this socket's real selection, or the original physical route.
                let selected = int(&native_get(fd, libc::IPPROTO_IPV6, libc::IPV6_MULTICAST_IF)?) as u32;
                if selected != 0 { selected } else { uplink::current().ok_or(ENODEV)?.index }
            };
            request[16..20].copy_from_slice(&host.to_ne_bytes());
            let option = if option == 20 { libc::IPV6_JOIN_GROUP } else { libc::IPV6_LEAVE_GROUP };
            native_set(fd, libc::IPPROTO_IPV6, option, &request)
        }
        _ => unreachable!(),
    }
}

pub(super) fn set(input: &SocketInput, level: u64, option: u64, address: u64, length: u32) -> Option<i64> {
    if !matches!((level, option), (V4, 32..=36) | (V6, 17..=21)) { return None; }
    Some(match set_value(input, level, option, address, length) { Ok(()) => 0, Err(error) => -(error as i64) })
}

fn get_value(input: &SocketInput, level: u64, option: u64, address: u64, length: u64) -> Result<(), Errno> {
    let capacity = int(&read(length, 4)?);
    if capacity < 0 { return Err(EINVAL); }
    let fd = input.fd();
    let (bytes, scalar) = match (level, option) {
        (V4, 32) => {
            let host = native_get(fd, libc::IPPROTO_IP, libc::IP_MULTICAST_IF)?;
            let value = any_sock(input).and_then(|socket| match &socket.family {
                Family::Inet(owner) => Some(owner.multicast_v4_address.load(Ordering::Relaxed).to_ne_bytes()), _ => None,
            }).unwrap_or(host);
            (value, false)
        }
        (V4, 33 | 34) => {
            let host = if option == 33 { libc::IP_MULTICAST_TTL } else { libc::IP_MULTICAST_LOOP };
            let value = native_get(fd, libc::IPPROTO_IP, host)?;
            ((value[0] as i32).to_ne_bytes(), true)
        }
        (V6, 17..=19) => {
            let host = match option { 17 => libc::IPV6_MULTICAST_IF, 18 => libc::IPV6_MULTICAST_HOPS, _ => libc::IPV6_MULTICAST_LOOP };
            let mut value = native_get(fd, libc::IPPROTO_IPV6, host)?;
            if option == 17 {
                let default = any_sock(input).is_some_and(|socket| match &socket.family {
                    Family::Inet(owner) => owner.multicast_v6_default.load(Ordering::Relaxed), _ => false,
                });
                value = if default { 0u32.to_ne_bytes() } else { guest_index(int(&value) as u32)?.to_ne_bytes() };
            }
            (value, false)
        }
        _ => unreachable!(),
    };
    let count = if scalar && capacity > 0 && capacity < 4 { 1 } else { (capacity as usize).min(4) };
    // Prepare both outputs before changing either guest record.
    user_memory::prepare_write(address, count)?;
    user_memory::prepare_write(length, 4)?;
    user_memory::write_exact(address, &bytes[..count])?;
    user_memory::write_exact(length, &(count as u32).to_ne_bytes())
}
pub(super) fn get(input: &SocketInput, level: u64, option: u64, address: u64, length: u64) -> Option<i64> {
    if !matches!((level, option), (V4, 32..=34) | (V6, 17..=19)) { return None; }
    Some(match get_value(input, level, option, address, length) { Ok(()) => 0, Err(error) => -(error as i64) })
}
