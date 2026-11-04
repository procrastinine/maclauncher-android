import fs from 'node:fs';
import path from 'node:path';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';

/** Keep the original limit/credential owners; adapt the physical FD ceiling. */
export function prepareResourceLimits(aim){
 const root=path.join(aim,'crates/aim-linux-abi/src');
 const edit=(name,changes)=>{const file=path.join(root,name);let source=fs.readFileSync(file,'utf8');
  for(const[before,after]of changes){if(source.split(before).length!==2)throw Error('Original descriptor-limit boundary changed: '+name+' '+before.slice(0,100));source=source.replace(before,after);}
  writeChanged(file,source);
 };
 copyChanged(path.join(import.meta.dirname,'native/resource-limits.rs'),path.join(root,'resource-limits.rs'));
 copyChanged(path.join(import.meta.dirname,'native/descriptor-pins.rs'),path.join(root,'sys/descriptor-pins.rs'));
 edit('lib.rs',[['pub mod errno;','pub mod errno;\n#[path="resource-limits.rs"]\nmod resource_limits;']]);
 edit('sys/process.rs',[
  [`            user_write!(
                old,
                [u64; 2],
                [lim_from_host(cur.rlim_cur), lim_from_host(cur.rlim_max)]
            );`,
   '            let limits=match crate::resource_limits::current(res,(lim_from_host(cur.rlim_cur),lim_from_host(cur.rlim_max))){Ok(value)=>value,Err(error)=>return error};\n            user_write!(old,[u64;2],[limits.0,limits.1]);'],
  ['            let [c, m] = user_read!(new, [u64; 2]);',
   '            let [c, m] = user_read!(new, [u64; 2]);\n            if let Err(error)=crate::resource_limits::request(res,m){return error;}'],
 ]);
 edit('sys/cred.rs',[
  ['    if let Some([soft, hard]) = new {',
   '    let current=crate::resource_limits::current(res as u64,(current[0],current[1])).map(|limits|[limits.0,limits.1])?;\n    if let Some([soft, hard]) = new {'],
  ['        with(|id| id.rlimits[res] = Some((soft, hard)));',
   '        crate::resource_limits::request(res as u64,hard)?;\n        if res==7 {let limits=[soft,hard];let result=super::fdtab::descriptor_pins::change(soft,||super::process::prlimit(261,[0,res as u64,limits.as_ptr() as u64,0,0,0]));if result<0{return Err(result);}}\n        with(|id| id.rlimits[res] = Some((soft, hard)));'],
  ['        if !matches!(res, RLIMIT_DATA | 8 | RLIMIT_AS) {',
   '        if !matches!(res, RLIMIT_DATA | 7 | 8 | RLIMIT_AS) {'],
 ]);
 edit('sys/fdtab.rs',[
  ['pub struct Pinned {\n    descriptor: Option<aim_storage::private_fd::PrivateFd>,\n    kind: Option<Kind>,\n    socket_allowed: bool,\n    fuse: Option<Arc<super::fuse_client::Open>>,\n    borrowed: Option<Borrow>,\n    regular_access: Option<i32>,\n}',
   '#[path="descriptor-pins.rs"] pub(crate) mod descriptor_pins;\npub struct Pinned {\n    descriptor: Option<aim_storage::private_fd::PrivateFd>,\n    kind: Option<Kind>,\n    socket_allowed: bool,\n    fuse: Option<Arc<super::fuse_client::Open>>,\n    borrowed: Option<Borrow>,\n    regular_access: Option<i32>,\n    retained:Option<Arc<descriptor_pins::Owner>>,\n}'],
  ['        match &self.borrowed {\n            Some(borrow) => unsafe { std::os::fd::BorrowedFd::borrow_raw(borrow.0) },\n            None => self.descriptor.as_ref().unwrap().as_fd(),\n        }',
   '        if let Some(owner)=&self.retained{return unsafe{std::os::fd::BorrowedFd::borrow_raw(owner.fd())};}\n        match &self.borrowed{Some(borrow)=>unsafe{std::os::fd::BorrowedFd::borrow_raw(borrow.0)},None=>self.descriptor.as_ref().unwrap().as_fd()}'],
  ['if self.borrowed.is_some() {\n            return;\n        }','    if self.borrowed.is_some()||self.retained.is_some(){return;}'],
  ['install_storage_registrar().map_err(|_| crate::errno::EIO)?;\n    let mut kind=None;',
   '    install_storage_registrar().map_err(|_|crate::errno::EIO)?;\n    if let Some(pin)=descriptor_pins::pin(fd)?{return Ok(pin);}\n    let mut kind=None;'],
  ['let held = unsafe { libc::fcntl(fd, libc::F_DUPFD_CLOEXEC, hidden_base()) };\n        if held < 0 {\n            return Err(std::io::Error::last_os_error());\n        }',
   '        let mut held=unsafe{libc::fcntl(fd,libc::F_DUPFD_CLOEXEC,hidden_base())};\n        if held<0&&matches!(std::io::Error::last_os_error().raw_os_error(),Some(libc::EINVAL)|Some(libc::EMFILE)){held=unsafe{libc::fcntl(fd,libc::F_DUPFD_CLOEXEC,0)};}\n        if held<0{return Err(std::io::Error::last_os_error());}'],
  ['})\n    .map_err(|error| crate::errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO)))?;\n    {\n        use std::os::fd::AsRawFd;\n        if let Some(file) = &fuse',
   '    });\n    let descriptor=match descriptor{Ok(descriptor)=>descriptor,Err(error)=>{if let Some(pin)=descriptor_pins::pin(fd)?{return Ok(pin);}return Err(crate::errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO)));}};\n    {use std::os::fd::AsRawFd;if let Some(file)=&fuse'],
  ['        regular_access: None,','        regular_access: None,\n        retained:None,'],
  ['        regular_access: access,','        regular_access: access,\n        retained:None,'],
  ['    let paths=RETIRED_PATHS.with(|owners|owners.borrow_mut().take());','    let paths=RETIRED_PATHS.with(|owners|owners.borrow_mut().take());\n    let retained=descriptor_pins::end();'],
  ['    drop(retired);drop(paths);','    drop(retired);drop(paths);drop(retained);'],
  ['    RETIRED_PATHS.with(|paths|{assert!(paths.borrow().is_none());*paths.borrow_mut()=Some(Vec::new());});','    RETIRED_PATHS.with(|paths|{assert!(paths.borrow().is_none());*paths.borrow_mut()=Some(Vec::new());});\n    descriptor_pins::begin();'],
  ['    clear_regular_access(fd);\n    super::fuse_client::close(fd);','    clear_regular_access(fd);\n    descriptor_pins::retire(fd);\n    super::fuse_client::close(fd);'],
  ['}\n    Ok(records.join("\\n"))','    }records.extend(descriptor_pins::exec_records()?);Ok(records.join("\\n"))'],
  ['pub(crate) struct ExecWriterFlags(Vec<(Kind,i32,i32)>,Vec<(Arc<OpenedPath>,i32)>,Vec<(Arc<ashmem_access::Access>,i32,i32)>);',
   'pub(crate) struct ExecWriterFlags(Vec<(Kind,i32,i32)>,Vec<(Arc<OpenedPath>,i32)>,Vec<(Arc<ashmem_access::Access>,i32,i32)>,descriptor_pins::ExecFlags);'],
  ['    let mut changed=ExecWriterFlags(Vec::new(),Vec::new(),Vec::new());',
   '    let mut changed=ExecWriterFlags(Vec::new(),Vec::new(),Vec::new(),descriptor_pins::prepare_exec()?);'],
  ['    let mut inherited = Vec::new();let mut accesses=Vec::new();',
   '    let mut inherited = Vec::new();let mut accesses=Vec::new();let mut limit_pins=Vec::new();'],
  ['        if fields.first()==Some(&"ashmem-access"){',
   '        if fields.first()==Some(&"limit-pin"){\n            if fields.len()!=3{return Err(crate::errno::EINVAL);}\n            limit_pins.push((fields[1].parse().map_err(|_|crate::errno::EINVAL)?,fields[2].parse().map_err(|_|crate::errno::EINVAL)?));continue;\n        }\n        if fields.first()==Some(&"ashmem-access"){'],
  ['    for fd in inherited {\n        let _guard = lifecycle();\n        unhide(fd);',
   '    descriptor_pins::restore_exec(&limit_pins)?;\n    for fd in inherited {\n        let _guard = lifecycle();\n        unhide(fd);'],
 ]);
}
