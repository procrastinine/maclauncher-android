// Compile a private executable file object before replacing a public mapping.
// Kernel mapping failure retains its ordinary errno; translation refusal never
// destroys the previous MAP_FIXED object or changes its bytes/protection.
fn preflight_executable_map(address:u64,length:u64,protection:i32,fd:i32,offset:u64,source:&ExecSource)->i64 {
    struct Staging(u64,u64);
    impl Drop for Staging{fn drop(&mut self){unsafe{libc::munmap(self.0 as*mut _,self.1 as usize);}}}
    let staging=match host_mmap(0,length,libc::PROT_READ|libc::PROT_WRITE,libc::MAP_PRIVATE|libc::MAP_ANON,-1,0){Ok(base)=>Staging(base,length),Err(error)=>return error};
    if let Err(error)=populate(staging.0,length,fd,offset){return error;}
    let bytes=unsafe{std::slice::from_raw_parts(staging.0 as*const u8,length as usize)};
    if let Err(error)=patch::check_original_instructions(address,bytes){return -(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64);}
    let prepared=match source {
        ExecSource::LoadTime(Some(sites))=>{
            let targets:Vec<_>=sites.sites.iter().filter(|&&(at,_,_)|at>=offset&&at+4<=offset+length)
                .map(|&(at,kind,word)|(address+at-offset,kind,word)).collect();
            patch::prepare_rewrite_sites(&targets,address,address+length,true)
        },
        ExecSource::LoadTime(None)=>patch::prepare_region_at(address,bytes,true,"elf-map"),
        ExecSource::Shared=>unreachable!(),
    };
    let mut prepared=match prepared{Ok(prepared)=>prepared,Err(error)=>return -(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64)};
    for &(site,word)in &prepared.words{unsafe{((staging.0+site-address)as*mut u32).write(word);}}
    let fips=if let ExecSource::LoadTime(Some(sites))=source {
        if let Some(module)=&sites.fips{
            match prepare_fips_hash(module,address,staging.0,length,fd,offset){Ok(hash)=>hash.map(|(at,hash)|(at,module.original,hash)),Err(error)=>return error}
        }else{None}
    }else{None};
    let protection_result=host_mprotect(staging.0,length,protection);if protection_result<0{return protection_result;}
    unsafe{sys_icache_invalidate(staging.0 as*mut _,length as usize);}
    // Make immutable trap identities visible before their instruction bytes.
    // Old identities remain process-owned, including an already fetched trap.
    prepared.publish();
    let(mut at,mut current,mut maximum)=(address,0,0);
    let result=unsafe{mach_vm_remap(task(),&mut at,length,0,VM_FLAGS_FIXED|VM_FLAGS_OVERWRITE,
        task(),staging.0,1,&mut current,&mut maximum,VM_INHERIT_COPY)};
    if result!=0{return -(ENOMEM as i64);}
    if at!=address||current&protection!=protection{return -(ENOMEM as i64);}
    patch::forget_far(address,length);prepared.commit();
    if let Some((at,original,hash))=fips{if !store_hash(at,&original,&hash){crate::diag!("[linux-abi] FIPS module hash at {at:#x} is not mapped from this file");}}
    crate::diag::register_fd_module(address,length,fd,offset);
    trace_stats("rewrote",address,length,&prepared.stats);
    copies::forget(address,address+length);super::fuse_cache::forget(address,length);
    note_file_copy(address,length,fd,offset);address as i64
}

fn prepare_fips_hash(module:&fips::Module,address:u64,staging:u64,length:u64,fd:i32,offset:u64)->Result<Option<(u64,fips::Hash)>,i64>{
    let size=module.text.1-module.text.0;
    if module.text_offset<offset||module.text_offset+size>offset+length{return Ok(None);}
    let rodata=if let Some((lo,hi))=module.rodata{
        use std::os::fd::BorrowedFd;
        let descriptor=unsafe{BorrowedFd::borrow_raw(fd)};
        let description=super::regular_file::adopt_kernel(descriptor,0).map_err(|error|-(error as i64))?;
        let source=super::verified_source::VerifiedSource::from_descriptor(descriptor,description).map_err(|error|-(error as i64))?;
        let mut bytes=vec![0;(hi-lo)as usize];source.read_exact_at(&mut bytes,module.rodata_offset).map_err(|error|-(error as i64))?;Some(bytes)
    }else{None};
    let text=unsafe{std::slice::from_raw_parts((staging+module.text_offset-offset)as*const u8,size as usize)};
    let hash=fips::digest(text,rodata.as_deref());
    Ok(Some(((address+module.text_offset-offset-module.text.0).wrapping_add(module.hash_vaddr),hash)))
}

fn validate_fixed_executable_file(address:u64,length:u64,fd:i32,offset:u64)->Result<(),i64>{
    let sites=patch::far_history_sites(address,length);if sites.is_empty(){return Ok(());}
    use std::os::fd::BorrowedFd;
    let descriptor=unsafe{BorrowedFd::borrow_raw(fd)};
    let description=super::regular_file::adopt_kernel(descriptor,0).map_err(|error|-(error as i64))?;
    let source=super::verified_source::VerifiedSource::from_descriptor(descriptor,description).map_err(|error|-(error as i64))?;
    for site in sites{let mut word=[0u8;4];source.read_exact_at(&mut word,offset+site-address).map_err(|error|-(error as i64))?;
        patch::check_original_instructions(site,&word).map_err(|error|-(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64))?;}
    Ok(())
}
