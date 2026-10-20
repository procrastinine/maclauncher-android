import fs from 'node:fs';import path from 'node:path';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';

/** Preserve native I/O while staging kernel copyout into owned JIT buffers. */
export function prepareUserOutput(aim){
 const root=path.join(aim,'crates/aim-linux-abi/src');
 const edit=(name,changes)=>{const file=path.join(root,name);let text=fs.readFileSync(file,'utf8');for(const[before,after]of changes){if(text.split(before).length!==2)throw Error('Original user-output boundary changed: '+name+' '+before.slice(0,70));text=text.replace(before,after);}writeChanged(file,text);};
 copyChanged(path.join(import.meta.dirname,'native/user-output.rs'),path.join(root,'sys/user-output.rs'));
 edit('sys/mod.rs',[['pub(crate) mod user_memory;','pub(crate) mod user_memory;\n#[path="user-output.rs"]\npub(crate) mod user_output;']]);
 edit('sys/regular_file.rs',[
  ['unsafe extern "C"{static mach_task_self_:u32;fn mach_vm_write(task:u32,address:u64,data:usize,length:u32)->i32;}',''],
  ['   let result=unsafe{match(position,write){','   if !write{let result=super::user_output::readv(fd.as_raw_fd(),iov,position,true);return if result<0{Err((-result)as Errno)}else{Ok(result)};}\n   let result=unsafe{match(position,write){'],
  ['    let result=unsafe{mach_vm_write(mach_task_self_,(target.iov_base as u64)+copied as u64,bytes.as_ptr()as usize,bytes.len()as u32)};\n    if result!=0{return if total>0{Ok(total)}else{Err(14)}};',
   '    if let Err(error)=super::user_memory::write_prepared((target.iov_base as u64)+copied as u64,&bytes){return if total>0{Ok(total)}else{Err(error)}};'],
 ]);
 edit('sys/regular_file.rs',[
  ['        assert_eq!(u64::from_le_bytes(bytes), 2);',
   '        assert_eq!(u64::from_le_bytes(bytes), 2);\n        super::super::signal::install_host_handlers();\n        let size=super::super::mem::PAGE as usize;\n        let jit=super::super::arena::map(super::super::arena::LO,size as u64,7,libc::MAP_PRIVATE|libc::MAP_ANON|libc::MAP_JIT,-1,0).unwrap();\n        let iov=libc::iovec{iov_base:jit as*mut _,iov_len:8};\n        super::super::user_memory::prepare_write(jit,8).unwrap();\n        assert_eq!(description.rw(source.as_fd(),&[iov],Some(0),false),8);\n        assert_eq!(super::super::user_memory::read_exact(jit,8).unwrap(),[0u8;8]);\n        assert_eq!(description.seek(source.as_fd(),0,libc::SEEK_CUR),24);\n        unsafe{libc::munmap(jit as*mut _,size);}'],
 ]);
 edit('sys/ptrace/agent.rs',[
  ['unsafe { mach_vm_write(mach_task_self_, addr, src.as_ptr() as usize, len as u32) == 0 }','super::super::user_memory::write_exact(addr,src).is_ok()'],
  ['    fn mach_vm_write(t: libc::mach_port_t, a: u64, d: usize, n: u32) -> i32;',''],
 ]);
 edit('sys/mem.rs',[
  ['            if unsafe { mach_vm_read_overwrite(task(), src, n, dst, &mut got) } != 0 {','            if !super::user_output::copy_memory(src,dst,n) {'],
  ['            let mut got = 0u64;\n            // SAFETY: a fault-safe copy within our own task.','            // SAFETY: a fault-safe copy within our own task.'],
 ]);
 edit('sys/fs.rs',[
  ['errno::check(unsafe{libc::readv(actual,iov.as_ptr(),iov.len() as i32)} as i64)','super::user_output::readv(actual,iov,None,false)'],
  ['errno::check(unsafe{if write{libc::pwrite(actual,buf as *const _,len,pos)}else{libc::pread(actual,buf as *mut _,len,pos)}} as i64)','if write{errno::check(unsafe{libc::pwrite(actual,buf as *const _,len,pos)}as i64)}else{super::user_output::read(actual,buf,len,Some(pos))}'],
  ['errno::check(\n            unsafe { libc::pread(fd, buf as *mut _, len, pos) } as i64,\n        )','super::user_output::read(fd,buf,len,Some(pos))'],
  ['errno::check(unsafe { libc::read(fd, buf as *mut _, len) } as i64)','super::user_output::read(fd,buf,len,None)'],
  ['errno::check(\n        unsafe { libc::pread(a[0] as i32, a[1] as *mut _, a[2] as usize, a[3] as i64) } as i64,\n    )','super::user_output::read(a[0]as i32,a[1],a[2]as usize,Some(a[3]as i64))'],
  ['errno::check(unsafe { libc::readv(a[0] as i32, v.as_ptr(), v.len() as i32) } as i64)','super::user_output::readv(a[0]as i32,v,None,false)'],
  ['errno::check(unsafe{libc::readv(actual,vectors.as_ptr(),vectors.len()as i32)}as i64)','super::user_output::readv(actual,vectors,None,false)'],
  ['errno::check(unsafe{if write{libc::pwrite(actual,vector.iov_base,vector.iov_len,pos+total)}else{libc::pread(actual,vector.iov_base,vector.iov_len,pos+total)}}as i64)','if write{errno::check(unsafe{libc::pwrite(actual,vector.iov_base,vector.iov_len,pos+total)}as i64)}else{super::user_output::read(actual,vector.iov_base as u64,vector.iov_len,Some(pos+total))}'],
 ]);
 edit('sys/net.rs',[
  ['fn recv(\n    input: &SocketInput,\n    iov: &[libc::iovec],\n    want_name: bool,\n    ctrl_cap: usize,\n    flags: u64,\n) -> Result<Received, i64> {',
   'fn recv(input:&SocketInput,iov:&[libc::iovec],want_name:bool,ctrl_cap:usize,flags:u64)->Result<Received,i64>{\n    let output=super::user_output::Output::new(iov).map_err(|error|-(error as i64))?;\n    let mut received=recv_owned(input,output.vectors(),want_name,ctrl_cap,flags)?;\n    let count=output.finish(received.n);if count<0{return Err(count); }received.n=count;Ok(received)\n}\n\nfn recv_owned(\n    input: &SocketInput,\n    iov: &[libc::iovec],\n    want_name: bool,\n    ctrl_cap: usize,\n    flags: u64,\n) -> Result<Received, i64> {'],
  ['            let copy = len.min(cap);\n            if peek {','            let copy = len.min(cap);\n            let initialized;let complete;\n            if peek {'],
  ['                for p in prefix(iov, n.saturating_sub(FRAME)) {','                initialized=n.saturating_sub(FRAME).min(copy);complete=initialized==copy;\n                for p in prefix(iov, initialized) {'],
  ['                recv_exact(fd, prefix(iov, copy), 0)?;\n                let mut left = len - copy;','                initialized=recv_exact(fd, prefix(iov, copy), 0)?;\n                let mut left = len - initialized;'],
  ['                    left -= n;\n                }\n            }\n            Ok(Received {','                    left -= n;\n                }\n                complete=left==0;\n            }\n            Ok(Received {'],
  ['                n: if flags & L_MSG_TRUNC != 0 {\n                    len as i64\n                } else {\n                    copy as i64\n                },','                n: if flags & L_MSG_TRUNC != 0 && complete {\n                    len as i64\n                } else {\n                    initialized as i64\n                },'],
  ['                unsafe { std::ptr::copy_nonoverlapping(n.as_ptr(), m.name as *mut u8, k) };',
   '                if let Err(error)=super::user_memory::write_exact(m.name,&n[..k]){return -(error as i64);}'],
  ['    unsafe { (mp as *mut LinuxMsghdr).write_unaligned(m) };',
   '    let bytes=unsafe{std::slice::from_raw_parts((&m as*const LinuxMsghdr).cast::<u8>(),std::mem::size_of::<LinuxMsghdr>())};\n    if let Err(error)=super::user_memory::write_exact(mp,bytes){return -(error as i64);}'],
 ]);
}
