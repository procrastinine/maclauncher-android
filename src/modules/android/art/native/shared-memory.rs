// Included in mutable_memory. Shared executable views retain the original
// memory object and its file pager; no source byte is copied or rewritten.
pub(crate) struct SharedPrepared {start:u64,length:u64,protection:i32,backing:Arc<Backing>,staging:Option<Backing>,overwrite:bool}
fn original_alias(start:u64,length:u64,protection:i32)->std::io::Result<Arc<Backing>>{
    if let Some(address)=vm::alias(start,length,None,R|W){return Ok(Arc::new(Backing::new(address,length,false)));}
    if protection&W!=0{return Err(std::io::Error::from_raw_os_error(libc::EACCES));}
    let address=vm::alias(start,length,None,R).ok_or_else(||std::io::Error::from_raw_os_error(libc::ENOMEM))?;
    Ok(Arc::new(Backing::new(address,length,false)))
}
fn shared_bounds(start:u64,length:u64)->std::io::Result<()>{
    if length==0||start%PAGE!=0||length%PAGE!=0||start.checked_add(length).is_none(){return Err(std::io::Error::from_raw_os_error(libc::EINVAL));}Ok(())
}
pub(crate) fn prepare_shared(start:u64,length:u64,protection:i32)->std::io::Result<SharedPrepared>{
    shared_bounds(start,length)?;super::native_writer::initialize().map_err(std::io::Error::other)?;
    let backing=original_alias(start,length,protection)?;
    Ok(SharedPrepared{start,length,protection,backing,staging:None,overwrite:false})
}
pub(crate) fn prepare_shared_remap(start:u64,source:u64,length:u64,protection:i32,overwrite:bool)->std::io::Result<SharedPrepared>{
    shared_bounds(start,length)?;super::native_writer::initialize().map_err(std::io::Error::other)?;
    // Retain both aliases before changing the target. The read-only source is
    // outside the guest placement range, so a fixed target cannot consume the
    // temporary guest mapping from which it was prepared.
    let backing=original_alias(source,length,protection)?;
    let address=vm::alias(source,length,None,protection&R).ok_or_else(||std::io::Error::from_raw_os_error(libc::ENOMEM))?;
    Ok(SharedPrepared{start,length,protection,backing,staging:Some(Backing::new(address,length,false)),overwrite})
}
impl SharedPrepared{
    pub(crate) fn install(self,suspend:impl FnOnce(&mut dyn FnMut()->std::io::Result<()>)->std::io::Result<()>)->std::io::Result<()>{
        let mut owner=OWNER.lock().unwrap_or_else(|error|error.into_inner());
        let previous=owner.ranges.clone();remove(&mut owner,self.start,self.length);
        owner.ranges.insert(self.start,Region{end:self.start+self.length,offset:0,protection:self.protection,generation:NEXT.fetch_add(1,Ordering::AcqRel),backing:self.backing.clone()});
        let old=exchange(snapshot(&owner));
        let mut publish=||{
            if let Some(source)=&self.staging{
                let(mut address,mut current,mut maximum)=(self.start,0,0);
                let flags=vm::VM_FLAGS_FIXED|if self.overwrite{vm::VM_FLAGS_OVERWRITE}else{0};
                let kr=unsafe{vm::mach_vm_remap(vm::task(),&mut address,self.length,0,flags,vm::task(),source.address,0,&mut current,&mut maximum,0)};
                if kr!=0{return Err(std::io::Error::from_raw_os_error(if !self.overwrite&&super::vmmap::regions(self.start,self.start+self.length).next().is_some(){libc::EEXIST}else{libc::ENOMEM}));}
                assert_eq!(address,self.start);assert_eq!(current,self.protection&R);
            }else if unsafe{libc::mprotect(self.start as*mut _,self.length as usize,self.protection&R)}!=0{return Err(std::io::Error::last_os_error());}
            Ok(())
        };
        if let Err(error)=suspend(&mut publish){owner.ranges=previous;let failed=SNAPSHOT.swap(old,Ordering::SeqCst);LIVE.store(!owner.ranges.is_empty(),Ordering::Release);retire(failed);return Err(error);}
        retire(old);Ok(())
    }
}

#[cfg(test)]
mod shared_tests{
    use super::*;
    use std::{io::{Read,Seek,SeekFrom,Write},os::fd::AsRawFd};
    #[test]
    fn shared_backing_keeps_file_aliases_and_refused_fixed_publication(){
        let path=std::fs::canonicalize(std::env::temp_dir()).unwrap().join(format!("aim-shared-owner-{}",std::process::id()));
        struct Remove(std::path::PathBuf);impl Drop for Remove{fn drop(&mut self){let _=std::fs::remove_file(&self.0);}}let _remove=Remove(path.clone());
        let mut file=std::fs::OpenOptions::new().create_new(true).read(true).write(true).open(&path).unwrap();
        file.set_len(2*PAGE).unwrap();file.write_all(&[0x37;16]).unwrap();
        let source=unsafe{libc::mmap(std::ptr::null_mut(),(2*PAGE)as usize,R|W,libc::MAP_SHARED,file.as_raw_fd(),0)};assert_ne!(source,libc::MAP_FAILED);
        let other=unsafe{libc::mmap(std::ptr::null_mut(),(2*PAGE)as usize,R|W,libc::MAP_SHARED,file.as_raw_fd(),0)};assert_ne!(other,libc::MAP_FAILED);
        let target=unsafe{libc::mmap(std::ptr::null_mut(),(2*PAGE)as usize,R|W,libc::MAP_PRIVATE|libc::MAP_ANON,-1,0)};assert_ne!(target,libc::MAP_FAILED);
        unsafe{std::ptr::write_bytes(target,0x5a,(2*PAGE)as usize);}
        let source=source as u64;let target=target as u64;let other=other as u64;
        let _epoch=super::super::verity_pager::mutation();let _transaction=transaction();
        let error=prepare_shared_remap(target,source,2*PAGE,R|W|X,true).unwrap().install(|_|Err(std::io::Error::from_raw_os_error(libc::ENOMEM))).unwrap_err();
        assert_eq!(error.raw_os_error(),Some(libc::ENOMEM));assert_eq!(unsafe{*(target as*const u8)},0x5a);assert_eq!(vm::region(target).unwrap().2,R|W);assert!(mapping(target,1,0).is_none());
        prepare_shared_remap(target,source,2*PAGE,R|W|X,true).unwrap().install(|publish|publish()).unwrap();
        assert_eq!(vm::region(target).unwrap().2,R);assert!(!mapping(target,1,R).unwrap().private_origin);assert!(super::super::vmmap::info_at(target).unwrap().shared);
        let region=super::super::vmmap::region_at(target).unwrap();assert_eq!(region.file.as_ref().unwrap().0,path);
        write(target+8,&[0x48,0x59]).unwrap();assert_eq!(unsafe{std::slice::from_raw_parts((source+8)as*const u8,2)},[0x48,0x59]);assert_eq!(unsafe{std::slice::from_raw_parts((other+8)as*const u8,2)},[0x48,0x59]);
        assert_eq!(unsafe{libc::msync(target as*mut _,(2*PAGE)as usize,libc::MS_SYNC)},0);
        file.seek(SeekFrom::Start(8)).unwrap();let mut bytes=[0;2];file.read_exact(&mut bytes).unwrap();assert_eq!(bytes,[0x48,0x59]);
        file.seek(SeekFrom::Start(10)).unwrap();file.write_all(&[0x6a]).unwrap();assert_eq!(unsafe{*((target+10)as*const u8)},0x6a);
        let readonly=std::fs::File::open(&path).unwrap();let ro=unsafe{libc::mmap(std::ptr::null_mut(),PAGE as usize,R,libc::MAP_SHARED,readonly.as_raw_fd(),0)};assert_ne!(ro,libc::MAP_FAILED);
        let error=prepare_shared_remap(target,ro as u64,PAGE,R|W|X,true).err().expect("readonly alias admission");assert_eq!(error.raw_os_error(),Some(libc::EACCES));assert_eq!(unsafe{*((target+10)as*const u8)},0x6a);
        forget(target,2*PAGE);unsafe{libc::munmap(target as*mut _,(2*PAGE)as usize);libc::munmap(source as*mut _,(2*PAGE)as usize);libc::munmap(other as*mut _,(2*PAGE)as usize);libc::munmap(ro,PAGE as usize);}
        drop(readonly);drop(file);std::fs::remove_file(path).unwrap();
    }
}
