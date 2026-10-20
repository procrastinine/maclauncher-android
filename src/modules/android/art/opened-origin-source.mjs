import fs from 'node:fs';
import path from 'node:path';
import {writeChanged} from '../native/upstream/source-files.mjs';

/** Original translated bytes may be shared by many image names. Their cache
 * inode describes code, while the existing fdtable owns each opened name. */
export function prepareOpenedOrigins(aim){
 const replace=(relative,before,after)=>{
  const file=path.join(aim,'crates/aim-linux-abi/src',relative),source=fs.readFileSync(file,'utf8');
  if(source.split(before).length!==2)throw Error('Original opened-file origin boundary changed');
  writeChanged(file,source.replace(before,after));
 };
 replace('sys/fs.rs',`    if let Some(fd) = crate::xrt::open_translated(&r.host, &r.guest, hflags) {
        return fd as i64;
    }`,`    if let Some(fd) = crate::xrt::open_translated(&r.host, &r.guest, hflags) {
        if fd>=0 {
            if let Err(error)=fdtab::remember_virtual_path(fd,r.guest.clone()).and_then(|_|fdtab::publish_guest(fd)) {
                fdtab::on_close(fd);unsafe{libc::close(fd);}return -(error as i64);
            }
        }
        return fd as i64;
    }`);
 replace('sys/procfs.rs',`    if let Some(p) = crate::xrt::original_guest_path(fd) {
        return Ok(p);
    }
    if let Some(path)=fdtab::opened_path(fd){return path;}`,`    // The per-open owner is exact even when another image/APEX opens the
    // same content-addressed translated inode under a different Linux name.
    if let Some(path)=fdtab::opened_path(fd){return path;}
    if let Some(p) = crate::xrt::original_guest_path(fd) {
        return Ok(p);
    }`);
 replace('sys/fs.rs',`    if unsafe { libc::fstat(fd, &mut st) } < 0 {
        return Err(-(errno::last() as i64));
    }
    if super::ashmem::as_device(&mut st)`, `    if unsafe { libc::fstat(fd, &mut st) } < 0 {
        return Err(-(errno::last() as i64));
    }
    if crate::xrt::original_guest_path(fd).is_some() {
        if let Some(origin)=fdtab::opened_path(fd) {
            let guest=origin.map_err(|error|-(error as i64))?;
            let original=vfs::resolve(vfs::LINUX_AT_FDCWD,guest.as_bytes(),true).map_err(|error|-(error as i64))?;
            if original.read_only() {
                // Bionic realpath compares this identity with stat(name).
                // The immutable image owns the inode; the shared translated
                // code view still owns its expanded ELF size and read bytes.
                let mut source:libc::stat=unsafe{std::mem::zeroed()};
                if unsafe{libc::stat(original.host.as_ptr(),&mut source)}<0{return Err(-(errno::last() as i64));}
                st.st_dev=source.st_dev;st.st_ino=source.st_ino;
            }
        }
    }
    if super::ashmem::as_device(&mut st)`);
}
