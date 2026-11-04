import fs from 'node:fs';import path from 'node:path';
import{copyChanged,writeChanged}from'../native/upstream/source-files.mjs';

/** Full Linux datagram lengths over the existing socket allocation and wire record. */
export function prepareDatagramReceive(aim){
 const root=path.join(aim,'crates/aim-linux-abi/src/sys');
 const edit=(name,changes)=>{const file=path.join(root,name);let text=fs.readFileSync(file,'utf8');for(const[before,after]of changes){if(text.split(before).length!==2)throw Error('Original datagram receive boundary changed: '+name+' '+before.slice(0,70));text=text.replace(before,after);}writeChanged(file,text);};
 copyChanged(path.join(import.meta.dirname,'native/datagram-receive.rs'),path.join(root,'net/datagram_receive.rs'));
 edit('net.rs',[
  ['mod proc_table;','mod proc_table;\nmod datagram_receive;'],
  ['    recv: Mutex<()>,','    recv: Mutex<()>,\n    datagram_receive: Mutex<Option<Arc<datagram_receive::Admission>>>,'],
  ['            recv: Mutex::new(()),','            recv: Mutex::new(()),\n            datagram_receive: Mutex::new(None),'],
  ['pub(super) fn save_sock(s: &Sock,', 'pub(super) fn save_sock(s: &Arc<Sock>,'],
  ['    w.opt(*s.inet_owner.lock().unwrap(),', '    datagram_receive::save(s,w);\n    w.opt(*s.inet_owner.lock().unwrap(),'],
  ['    let inet_owner = r.opt(|r| SocketMetadata', '    let receive_fd=r.i32();\n    let inet_owner = r.opt(|r| SocketMetadata'],
  ['    *s.inet_owner.lock().unwrap() = inet_owner;', '    if let Err(error)=datagram_receive::restore(&s,receive_fd){eprintln!("fork datagram admission: errno {error}");r.invalidate();}\n    *s.inet_owner.lock().unwrap() = inet_owner;'],
  ['fn recv_owned(\n    input: &SocketInput,','fn recv_owned(input:&SocketInput,iov:&[libc::iovec],want_name:bool,ctrl_cap:usize,flags:u64)->Result<Received,i64>{\n    datagram_receive::receive(input,iov,want_name,ctrl_cap,flags)\n}\n\nfn recv_original(\n    input: &SocketInput,'],
 ]);
 edit('net/socket_inode.rs',[
  ['fn directory() -> PathBuf {','pub(super) fn directory()->PathBuf {'],
  ['*previous = Some(receipt);\n    Ok(())','    *previous=Some(receipt);drop(previous);super::datagram_receive::initialize(copy.as_raw_fd(),&socket)'],
  ['let mut owner = socket.inode_allocation.lock().unwrap();\n    if owner.is_some() {\n        return Err(EIO);\n    }\n    *owner = Some(receipt);\n    Ok(())','    let mut owner=socket.inode_allocation.lock().unwrap();if owner.is_some(){return Err(EIO);}*owner=Some(receipt);drop(owner);super::datagram_receive::initialize(copy.as_raw_fd(),&socket)'],
 ]);
 edit('fdtab.rs',[
  ['pub struct ForkPrivateFd {\n    held: Option<std::os::fd::OwnedFd>,\n    target: i32,\n}', 'pub struct ForkPrivateFd{held:Option<std::os::fd::OwnedFd>,retained:Option<Kind>,target:i32}'],
  ['pub fn source(&self) -> std::os::fd::BorrowedFd<\'_> {\n        use std::os::fd::AsFd;\n        self.held.as_ref().unwrap().as_fd()\n    }', 'pub fn source(&self)->std::os::fd::BorrowedFd<\'_>{use std::os::fd::AsFd;match &self.held{Some(fd)=>fd.as_fd(),None=>unsafe{std::os::fd::BorrowedFd::borrow_raw(self.target)}}}'],
  ['let held = self.held.take().unwrap();\n        if RETIRED.with', '    let Some(held)=self.held.take()else{retire(self.retained.take());return;};\n    if RETIRED.with'],
  ['Ok(ForkPrivateFd {\n        held: Some(unsafe { std::os::fd::OwnedFd::from_raw_fd(held) }),\n        target,\n    })', 'Ok(ForkPrivateFd{held:Some(unsafe{std::os::fd::OwnedFd::from_raw_fd(held)}),retained:None,target})'],
  ['pub(crate) fn fork_guest_snapshot()', 'pub(super) fn retain_fork_private(fd:i32,owner:Kind)->ForkPrivateFd{ForkPrivateFd{held:None,retained:Some(owner),target:fd}}\npub(crate) fn fork_guest_snapshot()'],
 ]);
 edit('fork/state.rs',[
  ['w.u32(0x46444e12);','w.u32(0x46444e13);'],
  ['if version != 0x46444e12 {','if version != 0x46444e13 {'],
  ['expected=0x46444e12','expected=0x46444e13'],
  ['    pub fn take_private(&mut self)', '    pub fn retain_owner(&mut self,fd:i32,owner:fdtab::Kind){if !self.1.iter().any(|held|held.target()==fd){self.1.push(fdtab::retain_fork_private(fd,owner));}}\n    pub fn take_private(&mut self)'],
 ]);
 edit('fork/spawn.rs',[
  ['if transfers.iter().any(|(_, target)| sources.contains(target))','if transfers.iter().any(|(source,target)|source!=target&&sources.contains(target))'],
  ['checked(if target >= 10240 {\n                posix_spawn_file_actions_addinherit_np', 'checked(if target>=10240||source==target{posix_spawn_file_actions_addinherit_np'],
 ]);
}
