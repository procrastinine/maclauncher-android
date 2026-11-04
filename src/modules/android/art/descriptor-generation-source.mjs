import fs from 'node:fs';import path from 'node:path';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';

/** Register a new native slot before installing its Linux open description. */
export function prepareDescriptorGenerations(aim){
 const root=path.join(aim,'crates/aim-linux-abi/src');
 const edit=(name,changes)=>{const file=path.join(root,name);let text=fs.readFileSync(file,'utf8');for(const[from,to]of changes){if(text.split(from).length!==2)throw Error('Descriptor generation boundary changed: '+name+' '+from.slice(0,90));text=text.replace(from,to);}writeChanged(file,text);};
 copyChanged(path.join(import.meta.dirname,'native/descriptor-generation.rs'),path.join(root,'sys/descriptor-generation.rs'));
 edit('sys/fdtab.rs',[
  ['static TABLE: LazyLock<RwLock<HashMap<i32, Entry>>> = LazyLock::new(Default::default);','static TABLE: LazyLock<RwLock<HashMap<i32, Entry>>> = LazyLock::new(Default::default);\ninclude!("descriptor-generation.rs");'],
  ['pub fn on_dup(old: i32, new: i32) {\n    on_close(new);','pub fn on_dup(old: i32, new: i32) {\n    begin_fresh(new).expect("dup producer owns its new native descriptor");'],
 ]);
 const fresh='    if let Err(error)=fdtab::begin_fresh(fd){unsafe{libc::close(fd);}return -(error as i64);}\n';
 edit('sys/fs.rs',[
  ['    if r.guest == "/dev/ptmx" && flags & O_PATH == 0 {',fresh+'    if r.guest == "/dev/ptmx" && flags & O_PATH == 0 {'],
  ['                errno::check(unsafe { libc::open(p.as_ptr(), open_flags_to_host(flags)) } as i64)',
   '                let fd=unsafe{libc::open(p.as_ptr(),open_flags_to_host(flags))};\n                if fd<0{return -(errno::last()as i64);}\n'+fresh+'                fd as i64'],
  ['    for fd in fds {\n        fdtab::set_flags(fd, flags & O_NONBLOCK != 0, flags & O_CLOEXEC != 0);',
   '    for fd in fds {\n        if let Err(error)=fdtab::begin_fresh(fd){for fd in fds{unsafe{libc::close(fd);}}return -(error as i64);}\n        fdtab::set_flags(fd, flags & O_NONBLOCK != 0, flags & O_CLOEXEC != 0);'],
 ]);
 edit('sys/net.rs',[
  ['    set_int(fd, libc::SOL_SOCKET, libc::SO_NOSIGPIPE, 1);\n    Ok(fd)',
   '    if let Err(error)=fdtab::begin_fresh(fd){unsafe{libc::close(fd);}return Err(-(error as i64));}\n    set_int(fd, libc::SOL_SOCKET, libc::SO_NOSIGPIPE, 1);\n    Ok(fd)'],
  ['            set_int(fd, libc::SOL_SOCKET, libc::SO_NOSIGPIPE, 1);\n            let groups = match super::bpf::socket_cgroup() {',
   '            if let Err(error)=fdtab::begin_fresh(fd){unsafe{libc::close(fd);}return -(error as i64);}\n            set_int(fd, libc::SOL_SOCKET, libc::SO_NOSIGPIPE, 1);\n            let groups = match super::bpf::socket_cgroup() {'],
  ['            fdtab::insert(\n                fd,\n                Kind::Sock(Sock::of(SockType::Dgram, Family::Netlink(n))),',
   fresh+'            fdtab::insert(\n                fd,\n                Kind::Sock(Sock::of(SockType::Dgram, Family::Netlink(n))),'],
  ['            fdtab::insert(fd, Kind::Sock(Sock::of(SockType::Dgram, Family::Packet(p))));',
   fresh+'            fdtab::insert(fd, Kind::Sock(Sock::of(SockType::Dgram, Family::Packet(p))));'],
  ['    for fd in fds {\n        set_int(fd, libc::SOL_SOCKET, libc::SO_NOSIGPIPE, 1);',
   '    for fd in fds {\n        if let Err(error)=fdtab::begin_fresh(fd){for fd in fds{unsafe{libc::close(fd);}}return -(error as i64);}\n        set_int(fd, libc::SOL_SOCKET, libc::SO_NOSIGPIPE, 1);'],
  ['    // Darwin accepted sockets inherit O_NONBLOCK; Linux\'s do not.',
   '    if let Err(error)=fdtab::begin_fresh(nfd){unsafe{libc::close(nfd);}return -(error as i64);}\n    // Darwin accepted sockets inherit O_NONBLOCK; Linux\'s do not.'],
  ['fn adopt_received_descriptor(fd: i32) -> Result<(), errno::Errno> {',
   'fn adopt_received_descriptor(fd: i32) -> Result<(), errno::Errno> {\n    fdtab::begin_fresh(fd)?;'],
 ]);
}
