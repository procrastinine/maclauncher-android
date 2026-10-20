import fs from 'node:fs';
import path from 'node:path';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';

/** Keep the original limit/credential owners; adapt the physical FD ceiling. */
export function prepareResourceLimits(aim){
 const root=path.join(aim,'crates/aim-linux-abi/src');
 const edit=(name,changes)=>{const file=path.join(root,name);let source=fs.readFileSync(file,'utf8');
  for(const[before,after]of changes){if(source.split(before).length!==2)throw Error('Original descriptor-limit boundary changed: '+name);source=source.replace(before,after);}
  writeChanged(file,source);
 };
 copyChanged(path.join(import.meta.dirname,'native/resource-limits.rs'),path.join(root,'resource-limits.rs'));
 copyChanged(path.join(import.meta.dirname,'native/descriptor-pins.rs'),path.join(root,'sys/descriptor-pins.rs'));
 edit('lib.rs',[['pub mod errno;','pub mod errno;\n#[path="resource-limits.rs"]\nmod resource_limits;']]);
 edit('sys/process.rs',[
  ['            (old as *mut [u64; 2])\n                .write_unaligned([lim_from_host(cur.rlim_cur), lim_from_host(cur.rlim_max)]);',
   '            let limits=match crate::resource_limits::current(res,(lim_from_host(cur.rlim_cur),lim_from_host(cur.rlim_max))){Ok(value)=>value,Err(error)=>return error};\n            (old as *mut [u64; 2]).write_unaligned([limits.0,limits.1]);'],
  ['            let [c, m] = (new as *const [u64; 2]).read_unaligned();',
   '            let [c, m] = (new as *const [u64; 2]).read_unaligned();\n            if let Err(error)=crate::resource_limits::request(res,m){return error;}'],
 ]);
 edit('sys/cred.rs',[
  ['    if new != 0 {\n        // SAFETY: guest struct rlimit64.',
   '    let current=match crate::resource_limits::current(res,current){Ok(value)=>value,Err(error)=>return error};\n    if new != 0 {\n        // SAFETY: guest struct rlimit64.'],
  ['        if soft > hard {\n            return -(EINVAL as i64);\n        }',
   '        if soft > hard {\n            return -(EINVAL as i64);\n        }\n        if let Err(error)=crate::resource_limits::request(res,hard){return error;}'],
  ['            id.rlimits[res_i] = Some((soft, hard));',
   '            if res==7 {\n                let limits=[soft,hard];\n                let result=super::fdtab::descriptor_pins::change(soft,||super::process::prlimit(261,[0,res,limits.as_ptr() as u64,0,0,0]));\n                if result<0{return result;}\n            }\n            id.rlimits[res_i] = Some((soft, hard));'],
  ['        super::process::prlimit(261, [0, res, new, 0, 0, 0]);',
   '        if res!=7 {super::process::prlimit(261, [0, res, new, 0, 0, 0]);}'],
  ['        unsafe { (old as *mut [u64; 2]).write_unaligned(limit) };',
   '        let limit=match crate::resource_limits::current(res,(limit[0],limit[1])){Ok(value)=>[value.0,value.1],Err(error)=>return error};\n        unsafe { (old as *mut [u64; 2]).write_unaligned(limit) };'],
 ]);
 edit('sys/fdtab.rs',[
  ['pub struct Pinned{descriptor:Option<aim_storage::private_fd::PrivateFd>,kind:Option<Kind>,socket_allowed:bool,fuse:Option<Arc<super::fuse_client::Open>>,borrowed:Option<Borrow>}',
   '#[path="descriptor-pins.rs"] pub(crate) mod descriptor_pins;\npub struct Pinned{descriptor:Option<aim_storage::private_fd::PrivateFd>,kind:Option<Kind>,socket_allowed:bool,fuse:Option<Arc<super::fuse_client::Open>>,borrowed:Option<Borrow>,retained:Option<Arc<descriptor_pins::Owner>>}'],
  ['        match &self.borrowed{Some(borrow)=>unsafe{std::os::fd::BorrowedFd::borrow_raw(borrow.0)},None=>self.descriptor.as_ref().unwrap().as_fd()}',
   '        if let Some(owner)=&self.retained{return unsafe{std::os::fd::BorrowedFd::borrow_raw(owner.fd())};}\n        match &self.borrowed{Some(borrow)=>unsafe{std::os::fd::BorrowedFd::borrow_raw(borrow.0)},None=>self.descriptor.as_ref().unwrap().as_fd()}'],
  ['    if self.borrowed.is_some(){return;}','    if self.borrowed.is_some()||self.retained.is_some(){return;}'],
  ['    install_storage_registrar().map_err(|_|crate::errno::EIO)?;\n    let mut kind=None;',
   '    install_storage_registrar().map_err(|_|crate::errno::EIO)?;\n    if let Some(pin)=descriptor_pins::pin(fd)?{return Ok(pin);}\n    let mut kind=None;'],
  ['        let held=unsafe{libc::fcntl(fd,libc::F_DUPFD_CLOEXEC,hidden_base())};\n        if held<0{return Err(std::io::Error::last_os_error());}',
   '        let mut held=unsafe{libc::fcntl(fd,libc::F_DUPFD_CLOEXEC,hidden_base())};\n        if held<0&&matches!(std::io::Error::last_os_error().raw_os_error(),Some(libc::EINVAL)|Some(libc::EMFILE)){held=unsafe{libc::fcntl(fd,libc::F_DUPFD_CLOEXEC,0)};}\n        if held<0{return Err(std::io::Error::last_os_error());}'],
  ['    }).map_err(|error|crate::errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO)))?;\n    {use std::os::fd::AsRawFd;if let Some(file)=&fuse',
   '    });\n    let descriptor=match descriptor{Ok(descriptor)=>descriptor,Err(error)=>{if let Some(pin)=descriptor_pins::pin(fd)?{return Ok(pin);}return Err(crate::errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO)));}};\n    {use std::os::fd::AsRawFd;if let Some(file)=&fuse'],
  ['Ok(Pinned{descriptor:Some(descriptor),kind,socket_allowed,fuse,borrowed:None})','Ok(Pinned{descriptor:Some(descriptor),kind,socket_allowed,fuse,borrowed:None,retained:None})'],
  ['Ok(Pinned{descriptor:None,kind,socket_allowed:false,fuse:None,borrowed:Some(borrow)})','Ok(Pinned{descriptor:None,kind,socket_allowed:false,fuse:None,borrowed:Some(borrow),retained:None})'],
  ['    let paths=RETIRED_PATHS.with(|owners|owners.borrow_mut().take());','    let paths=RETIRED_PATHS.with(|owners|owners.borrow_mut().take());\n    let retained=descriptor_pins::end();'],
  ['    drop(retired);drop(paths);','    drop(retired);drop(paths);drop(retained);'],
  ['    RETIRED_PATHS.with(|paths|{assert!(paths.borrow().is_none());*paths.borrow_mut()=Some(Vec::new());});','    RETIRED_PATHS.with(|paths|{assert!(paths.borrow().is_none());*paths.borrow_mut()=Some(Vec::new());});\n    descriptor_pins::begin();'],
  ['pub fn on_close(fd: i32) {\n    super::fuse_client::close(fd);','pub fn on_close(fd: i32) {\n    descriptor_pins::retire(fd);\n    super::fuse_client::close(fd);'],
  ['    }Ok(records.join("\\n"))','    }records.extend(descriptor_pins::exec_records()?);Ok(records.join("\\n"))'],
  ['pub(crate) struct ExecWriterFlags(Vec<(Kind,i32,i32)>,Vec<(Arc<OpenedPath>,i32)>,Vec<(Arc<ashmem_access::Access>,i32,i32)>);',
   'pub(crate) struct ExecWriterFlags(Vec<(Kind,i32,i32)>,Vec<(Arc<OpenedPath>,i32)>,Vec<(Arc<ashmem_access::Access>,i32,i32)>,descriptor_pins::ExecFlags);'],
  ['    let mut changed=ExecWriterFlags(Vec::new(),Vec::new(),Vec::new());',
   '    let mut changed=ExecWriterFlags(Vec::new(),Vec::new(),Vec::new(),descriptor_pins::prepare_exec()?);'],
  ['    let mut inherited=Vec::new();let mut accesses=Vec::new();\n    for line in text.lines(){',
   '    let mut inherited=Vec::new();let mut accesses=Vec::new();let mut limit_pins=Vec::new();\n    for line in text.lines(){'],
  ['        if fields.first()==Some(&"ashmem-access"){',
   '        if fields.first()==Some(&"limit-pin"){\n            if fields.len()!=3{return Err(crate::errno::EINVAL);}\n            limit_pins.push((fields[1].parse().map_err(|_|crate::errno::EINVAL)?,fields[2].parse().map_err(|_|crate::errno::EINVAL)?));continue;\n        }\n        if fields.first()==Some(&"ashmem-access"){'],
  ['    for fd in inherited{let _guard=lifecycle();if unsafe{libc::close(fd)}<0{return Err(crate::errno::last());}unhide(fd);super::fd_visibility::private_closed(fd);}',
   '    descriptor_pins::restore_exec(&limit_pins)?;\n    for fd in inherited{let _guard=lifecycle();if unsafe{libc::close(fd)}<0{return Err(crate::errno::last());}unhide(fd);super::fd_visibility::private_closed(fd);}'],
 ]);
}
