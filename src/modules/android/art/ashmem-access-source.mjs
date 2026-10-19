import fs from 'node:fs';
import path from 'node:path';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';

/** One native exact-file authority implementation, beside the original codec. */
export function prepareAshmemCapability(aim){
 const root=path.join(aim,'crates/aim-ashmem/src'),file=path.join(root,'lib.rs');
 copyChanged(path.join(import.meta.dirname,'../native/binder-rpc/ashmem-access.rs'),path.join(root,'capability.rs'));
 let source=fs.readFileSync(file,'utf8');
 if(source.includes('pub mod capability;'))throw Error('AIM ashmem capability was already projected');
 source+='\n#[link(name="sandbox")]\nunsafe extern "C" {}\npub mod capability;\n';
 const before='    // SAFETY: the generated name is NUL-terminated and this fd is owned.\n    if unsafe { libc::unlink(template.as_ptr().cast()) } < 0 {';
 if(source.split(before).length!==2)throw Error('AIM ashmem producer boundary changed');
 source=source.replace(before,`    let authority=capability::issue(fd,unsafe{std::ffi::CStr::from_ptr(template.as_ptr().cast())});
    if let Err(error)=authority {unsafe{libc::unlink(template.as_ptr().cast());}return Err(error);}
${before}`);
 writeChanged(file,source);
}

export function prepareAshmemAccess(aim){
 prepareAshmemCapability(aim);
 const root=path.join(aim,'crates/aim-linux-abi/src');
 const edit=(file,changes)=>{const target=path.join(root,file);let source=fs.readFileSync(target,'utf8');
  for(const[from,to]of changes){if(source.split(from).length!==2)throw Error('AIM ashmem attachment boundary changed: '+file+' '+from.slice(0,60));source=source.replace(from,to);}
  writeChanged(target,source);
 };
 copyChanged(path.join(import.meta.dirname,'native/ashmem-access.rs'),path.join(root,'sys/ashmem-access.rs'));
 edit('hostcall.rs',[['        if func==12{return crate::sys::transport_peer(args,len); }','        if func==15{return crate::sys::fdtab::ashmem_access::call(args,len); }\n        if func==12{return crate::sys::transport_peer(args,len); }']]);
 edit('sys/ashmem.rs',[
  ['        libc::unlink(tmpl.as_ptr().cast());',`        if let Err(error)=aim_ashmem::capability::issue(fd,std::ffi::CStr::from_ptr(tmpl.as_ptr().cast())) {
            libc::unlink(tmpl.as_ptr().cast());libc::close(fd);
            return Some(-(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64));
        }
        libc::unlink(tmpl.as_ptr().cast());`],
 ]);
 edit('sys/fdtab.rs',[
  ['struct Entry { kind:Option<Kind>, path:Option<Arc<OpenedPath>> }','struct Entry { kind:Option<Kind>, path:Option<Arc<OpenedPath>>, access:Option<Arc<ashmem_access::Access>> }\n#[path="ashmem-access.rs"] pub(crate) mod ashmem_access;'],
  ['static RETIRED_PATHS:std::cell::RefCell<Option<Vec<Arc<OpenedPath>>>>','static RETIRED_PATHS:std::cell::RefCell<Option<Vec<Entry>>>'],
  [`    if let Some(entry)=entry { retire(entry.kind); let mut path=entry.path;
        RETIRED_PATHS.with(|paths|if let Some(paths)=paths.borrow_mut().as_mut(){if let Some(path)=path.take(){paths.push(path);}});drop(path);
    }`,
   `    let mut entry=entry;
    RETIRED_PATHS.with(|entries|if let Some(entries)=entries.borrow_mut().as_mut(){if let Some(entry)=entry.take(){entries.push(entry);}});
    if let Some(entry)=entry{retire(entry.kind);drop(entry.path);drop(entry.access);}`],
  ['Entry{kind:None,path:Some(path)}','Entry{kind:None,path:Some(path),..Default::default()}'],
  ['    let paths=table.iter().filter(|(fd,_)|visible(**fd)).filter_map(|(fd,entry)|entry.path.as_ref().map(|path|(*fd,path))).collect::<Vec<_>>();','    ashmem_access::save(&table,w);\n    let paths=table.iter().filter(|(fd,_)|visible(**fd)).filter_map(|(fd,entry)|entry.path.as_ref().map(|path|(*fd,path))).collect::<Vec<_>>();'],
  ['    let mut paths=Vec::<Arc<OpenedPath>>::new();','    ashmem_access::restore(r);\n    let mut paths=Vec::<Arc<OpenedPath>>::new();'],
  ['        if let Some(origin)=&entry.path{','        if let Some(access)=&entry.access{records.push(ashmem_access::exec_record(*fd,access));}\n        if let Some(origin)=&entry.path{'],
  ['pub(crate) struct ExecWriterFlags(Vec<(Kind,i32,i32)>,Vec<(Arc<OpenedPath>,i32)>);','pub(crate) struct ExecWriterFlags(Vec<(Kind,i32,i32)>,Vec<(Arc<OpenedPath>,i32)>,Vec<(Arc<ashmem_access::Access>,i32,i32)>);'],
  ['for(path,flags)in &self.1{unsafe{libc::fcntl(path.descriptor(),libc::F_SETFD,*flags);}}','for(path,flags)in &self.1{unsafe{libc::fcntl(path.descriptor(),libc::F_SETFD,*flags);}}for(_,fd,flags)in &self.2{unsafe{libc::fcntl(*fd,libc::F_SETFD,*flags);}}'],
  ['    let mut changed=ExecWriterFlags(Vec::new(),Vec::new());',`    let mut changed=ExecWriterFlags(Vec::new(),Vec::new(),Vec::new());
    let accesses=TABLE.read().unwrap().iter().filter(|(fd,_)|visible(**fd)&&unsafe{libc::fcntl(**fd,libc::F_GETFD)}&libc::FD_CLOEXEC==0).filter_map(|(_,entry)|entry.access.clone()).collect::<Vec<_>>();
    for access in accesses{if changed.2.iter().any(|(old,_,_)|Arc::ptr_eq(old,&access)){continue;}let(fd,flags)=ashmem_access::proof_flags(&access)?;changed.2.push((access,fd,flags));}`],
  ['    let mut inherited=Vec::new();\n    for line in text.lines(){','    let mut inherited=Vec::new();let mut accesses=Vec::new();\n    for line in text.lines(){'],
  ["        if fields.first()==Some(&\"origin\"){",`        if fields.first()==Some(&"ashmem-access"){
            if fields.len()!=4{return Err(crate::errno::EINVAL);}
            let fd=fields[1].parse().map_err(|_|crate::errno::EINVAL)?;
            let carrier=fields[2].parse().map_err(|_|crate::errno::EINVAL)?;
            let handle=fields[3].parse().map_err(|_|crate::errno::EINVAL)?;
            ashmem_access::restore_exec(fd,carrier,handle,&mut accesses)?;
            if !inherited.contains(&carrier){inherited.push(carrier);}continue;
        }
        if fields.first()==Some(&"origin"){`],
 ]);
 const state=path.join(root,'sys/fork/state.rs'),text=fs.readFileSync(state,'utf8');
 if(text.split('0x46444e04').length!==4)throw Error('AIM descriptor fork state boundary changed');
 writeChanged(state,text.replaceAll('0x46444e04','0x46444e05'));
}
