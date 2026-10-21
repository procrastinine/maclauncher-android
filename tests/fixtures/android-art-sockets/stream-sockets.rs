// Compiled inside the complete adapted aim-linux-abi net module.
// Every syscall, descriptor pin, receipt and transport owner is the real one.
use super::*;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};

fn retire(fd: i32) {
    fdtab::withdraw_guest(fd).unwrap();
    fdtab::on_close(fd);
}

fn adopt_stream(fd: i32) {
    adopt(fd);
    fdtab::publish_guest(fd).unwrap();
    assert!(matches!(fdtab::get(fd), Some(Kind::Sock(socket))
        if socket.ty == SockType::Stream));
}

fn stream_connect_sequence() {
    let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    let mut address = [0u8; 16];
    address[..2].copy_from_slice(&2u16.to_ne_bytes());
    address[2..4].copy_from_slice(&port.to_be_bytes());
    address[4..8].copy_from_slice(&[127, 0, 0, 1]);
    let mut in_progress = 0;
    for blocking_retry in [false, true] {
        let fd = unsafe { libc::socket(libc::AF_INET, libc::SOCK_STREAM, 0) };
        assert!(fd >= 0);
        let client = unsafe { OwnedFd::from_raw_fd(fd) };
        assert_eq!(unsafe { libc::fcntl(fd, libc::F_SETFL, libc::O_NONBLOCK) }, 0);
        adopt_stream(fd);
        let call = || connect([fd as u64, address.as_ptr() as u64, 16, 0, 0, 0]);
        let first = call();
        assert!(first == 0 || first == -115, "first connect: {first}");
        if first == -115 {
            in_progress += 1;
            if blocking_retry {
                assert_eq!(unsafe { libc::fcntl(fd, libc::F_SETFL, 0) }, 0);
            } else {
                let mut poll = libc::pollfd { fd, events: libc::POLLOUT, revents: 0 };
                assert_eq!(unsafe { libc::poll(&mut poll, 1, 5000) }, 1);
                assert_ne!(poll.revents & libc::POLLOUT, 0);
            }
            assert_eq!(call(), 0, "first repeated connect reports the connection");
        }
        assert_eq!(call(), -106, "a later connect reports Linux EISCONN");
        let (peer, _) = listener.accept().unwrap();
        assert_eq!(unsafe { libc::write(fd, b"c".as_ptr().cast(), 1) }, 1);
        let mut byte = 0u8;
        assert_eq!(unsafe { libc::read(peer.as_raw_fd(), (&mut byte as *mut u8).cast(), 1) }, 1);
        assert_eq!(byte, b'c');
        retire(fd);
        drop(client);
    }
    assert!(in_progress > 0, "the real Darwin EINPROGRESS path was exercised");
    println!("stream connect: EINPROGRESS -> 0 -> EISCONN; blocking retry; payload survives");
}

fn inherited_scm_socket_survives() {
    let name = format!("dev.maclauncher.test.stream-sockets.{}", std::process::id());
    let _server = aim_binder_host::server::Server::start(&name).unwrap();
    super::super::binder::init(&name).unwrap();
    let (sender, receiver) = std::os::unix::net::UnixStream::pair().unwrap();
    let (original, observer) = std::os::unix::net::UnixStream::pair().unwrap();
    for fd in [sender.as_raw_fd(), receiver.as_raw_fd(), original.as_raw_fd()] {
        adopt_stream(fd);
    }
    let state = match fdtab::get(original.as_raw_fd()).unwrap() {
        Kind::Sock(socket) => socket,
        _ => panic!("inherited descriptor was not recognized"),
    };
    assert!(state.inode_allocation.lock().unwrap().is_none());
    let retained = fdtab::export_scm(original.as_raw_fd()).unwrap();
    assert!(matches!(retained, fdtab::ScmExport::Other(_)),
        "an inherited socket without a receipt travels as its real descriptor");

    let mut control = [0u8; 24];
    control[..8].copy_from_slice(&20u64.to_ne_bytes());
    control[8..12].copy_from_slice(&1i32.to_ne_bytes());
    control[12..16].copy_from_slice(&1i32.to_ne_bytes());
    control[16..20].copy_from_slice(&original.as_raw_fd().to_ne_bytes());
    let mut byte = b's';
    let iov = [libc::iovec { iov_base: (&mut byte as *mut u8).cast(), iov_len: 1 }];
    let mut message = LinuxMsghdr { name: 0, namelen: 0, _pad: 0,
        iov: iov.as_ptr() as u64, iovlen: 1, control: control.as_mut_ptr() as u64,
        controllen: 24, flags: 0, _pad2: 0 };
    assert_eq!(sendmsg([sender.as_raw_fd() as u64, &message as *const _ as u64, 0, 0, 0, 0]), 1);
    retire(original.as_raw_fd());
    drop(original);
    drop(retained);
    control.fill(0);
    byte = 0;
    assert_eq!(recvmsg([receiver.as_raw_fd() as u64, &mut message as *mut _ as u64,
        L_MSG_CMSG_CLOEXEC, 0, 0, 0]), 1);
    assert_eq!(byte, b's');
    assert_eq!(message.controllen, 24);
    let received = i32::from_ne_bytes(control[16..20].try_into().unwrap());
    let transferred = unsafe { OwnedFd::from_raw_fd(received) };
    assert_ne!(unsafe { libc::fcntl(received, libc::F_GETFD) } & libc::FD_CLOEXEC, 0);
    assert!(matches!(fdtab::get(received), Some(Kind::Sock(socket))
        if socket.inode_allocation.lock().unwrap().is_none()));
    assert_eq!(unsafe { libc::write(observer.as_raw_fd(), b"r".as_ptr().cast(), 1) }, 1);
    assert_eq!(super::super::fs::read([received as u64, &mut byte as *mut u8 as u64, 1, 0, 0, 0]), 1);
    assert_eq!(byte, b'r');

    // A guest-created socket retains the receipt route and its allocation.
    let allocated = socket([2, 2 | 0x80000, 0, 0, 0, 0]);
    assert!(allocated >= 0);
    assert!(matches!(fdtab::export_scm(allocated as i32).unwrap(), fdtab::ScmExport::Socket(_)));
    assert_eq!(super::super::fs::close([allocated as u64, 0, 0, 0, 0, 0]), 0);
    for fd in [sender.as_raw_fd(), receiver.as_raw_fd(), received] {
        retire(fd);
    }
    drop(transferred);
    println!("inherited socket: actual SCM_RIGHTS transfer survives sender close; allocated socket retains receipt");
}

pub(crate) fn run() {
    let root = std::path::PathBuf::from(std::env::args_os().nth(1).unwrap());
    let image = root.join("root");
    let runtime = root.join("runtime");
    std::fs::create_dir_all(&image).unwrap();
    std::fs::create_dir_all(&runtime).unwrap();
    crate::vfs::use_runtime(runtime);
    crate::vfs::init(&image, None).unwrap();
    fdtab::install_storage_registrar().unwrap();
    stream_connect_sequence();
    inherited_scm_socket_survives();
}
