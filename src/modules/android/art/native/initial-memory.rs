// Included inside mutable_memory: sparse initial execution uses the same
// mapping leases, VM epoch and retirement owner as later writable generations.
unsafe extern "C" {fn sys_icache_invalidate(address:*mut std::ffi::c_void,length:usize);}
pub(crate) struct InitialPrepared {start:u64,length:u64,staging:Backing,regions:Vec<(u64,Region)>}
pub(crate) fn prepare_initial(start:u64,bytes:&[u8],protections:&[i32],sites:&[u64])->std::io::Result<InitialPrepared>{
    let length=bytes.len()as u64;
    let invalid=||std::io::Error::from_raw_os_error(libc::EINVAL);
    if length==0||start%PAGE!=0||length%PAGE!=0||start.checked_add(length).is_none()||protections.len()!=bytes.len()/PAGE as usize{return Err(invalid());}
    let mut selected:Vec<_>=protections.iter().map(|protection|protection&(W|X)==(W|X)).collect();
    for &site in sites {
        if site<start||site.checked_add(4).is_none_or(|end|end>start+length){return Err(invalid());}
        let index=((site-start)/PAGE)as usize;
        if protections[index]&X==0{return Err(invalid());}selected[index]=true;
    }
    let address=unsafe{libc::mmap(std::ptr::null_mut(),bytes.len(),R|W,libc::MAP_ANON|libc::MAP_PRIVATE,-1,0)};
    if address==libc::MAP_FAILED{return Err(std::io::Error::last_os_error());}
    let staging=Backing::new(address as u64,length,false);
    // Independent logical runs must not accidentally share one VM object.
    // Guarded temporary allocations prevent anonymous-object coalescing with
    // adjacent data; only their populated interior is moved into staging.
    // Each source byte is copied once, before the single public remap.
    let mut partition=0;
    while partition<protections.len(){
        let begin=partition;let protection=protections[partition];let owned=selected[partition];partition+=1;
        while partition<protections.len()&&protections[partition]==protection&&selected[partition]==owned{partition+=1;}
        let offset=begin as u64*PAGE;let size=(partition-begin)as u64*PAGE;
        let temporary=unsafe{libc::mmap(std::ptr::null_mut(),(size+2*PAGE)as usize,libc::PROT_NONE,libc::MAP_ANON|libc::MAP_PRIVATE,-1,0)};
        if temporary==libc::MAP_FAILED{return Err(std::io::Error::last_os_error());}
        let buffer=Backing::new(temporary as u64,size+2*PAGE,false);
        let interior=buffer.address+PAGE;
        if unsafe{libc::mprotect(interior as*mut _,size as usize,R|W)}!=0{return Err(std::io::Error::last_os_error());}
        unsafe{std::ptr::copy_nonoverlapping(bytes.as_ptr().add(offset as usize),interior as*mut u8,size as usize);}
        vm::alias(interior,size,Some(staging.address+offset),R|W).ok_or_else(std::io::Error::last_os_error)?;
        drop(buffer);
    }
    let mut regions=Vec::new();let mut index=0;
    while index<selected.len(){
        if !selected[index]{index+=1;continue;}
        let begin=index;let protection=protections[index];index+=1;
        while index<selected.len()&&selected[index]&&protections[index]==protection{index+=1;}
        let offset=begin as u64*PAGE;let size=(index-begin)as u64*PAGE;
        let alias=vm::alias(staging.address+offset,size,None,R|W).ok_or_else(std::io::Error::last_os_error)?;
        regions.push((start+offset,Region{end:start+offset+size,offset:0,protection,generation:0,backing:Arc::new(Backing::new(alias,size,true))}));
    }
    // Prepare the complete protection layout before the single public remap.
    // Mach's returned current protection is only the intersection of segments.
    let mut index=0;
    while index<protections.len(){
        let begin=index;let protection=if selected[index]{protections[index]&R}else{protections[index]};index+=1;
        while index<protections.len()&&(if selected[index]{protections[index]&R}else{protections[index]})==protection{index+=1;}
        if unsafe{libc::mprotect((staging.address+begin as u64*PAGE)as*mut _,(index-begin)*PAGE as usize,protection)}!=0{return Err(std::io::Error::last_os_error());}
    }
    unsafe{sys_icache_invalidate(address,bytes.len());}
    Ok(InitialPrepared{start,length,staging,regions})
}
impl InitialPrepared {
    pub(crate) fn install(self,suspend:impl FnOnce(&mut dyn FnMut()->std::io::Result<()>)->std::io::Result<()>)->std::io::Result<()> {
        let mut owner=OWNER.lock().unwrap_or_else(|error|error.into_inner());let previous=owner.ranges.clone();
        remove(&mut owner,self.start,self.length);
        for(start,region)in &self.regions{let mut region=region.clone();region.generation=NEXT.fetch_add(1,Ordering::AcqRel);owner.ranges.insert(*start,region);}
        let old=exchange(snapshot(&owner));
        let mut remap=||{
            let(mut address,mut current,mut maximum)=(self.start,0,0);
            let kr=unsafe{vm::mach_vm_remap(vm::task(),&mut address,self.length,0,vm::VM_FLAGS_FIXED|vm::VM_FLAGS_OVERWRITE,vm::task(),self.staging.address,0,&mut current,&mut maximum,vm::VM_INHERIT_COPY)};
            if kr!=0{return Err(std::io::Error::from_raw_os_error(libc::ENOMEM));}
            assert_eq!(address,self.start);Ok(())
        };
        if let Err(error)=suspend(&mut remap){owner.ranges=previous;let failed=SNAPSHOT.swap(old,Ordering::SeqCst);LIVE.store(!owner.ranges.is_empty(),Ordering::Release);retire(failed);return Err(error);}
        retire(old);Ok(())
    }
}
#[cfg(test)]
mod initial_tests {
    use super::*;
    #[test]
    fn sparse_initial_publication_keeps_original_views_and_exact_generations(){
        let _transaction=transaction();reset();let length=4*PAGE;
        let public=unsafe{libc::mmap(std::ptr::null_mut(),length as usize,R|W,libc::MAP_PRIVATE|libc::MAP_ANON,-1,0)};assert_ne!(public,libc::MAP_FAILED);let base=public as u64;
        unsafe{std::ptr::write_volatile(public.cast::<u8>(),0x32);}
        assert!(crate::sys::initial_execution::private_data(base,PAGE),"unique original anonymous RW data");
        let alias=vm::alias(base,PAGE,None,R|W).unwrap();
        assert!(!crate::sys::initial_execution::private_data(base,PAGE),"an actual Mach alias cannot receive private-data authority");
        unsafe{libc::munmap(alias as*mut _,PAGE as usize);}
        let bytes=vec![0x37;length as usize];let protections=[R|X,R|X,R|X,R|W];
        prepare_initial(base,&bytes,&protections,&[base+PAGE+4]).unwrap().install(|publish|publish()).unwrap();
        assert!(crate::sys::initial_execution::private_data(base+3*PAGE,PAGE),"unrelated RW data remains a true unique object after mixed atomic remap");
        assert_eq!(vm::region(base).unwrap().2,R|X);assert_eq!(vm::region(base+PAGE).unwrap().2,R);assert_eq!(vm::region(base+2*PAGE).unwrap().2,R|X);
        assert!(!crate::sys::initial_execution::private_data(base+PAGE,PAGE),"owned source keeps its write barrier");
        assert!(!intent(base,X));assert!(intent(base+PAGE,X));assert!(!intent(base+PAGE,W));assert!(!intent(base+2*PAGE,X));
        assert_eq!(unsafe{std::slice::from_raw_parts(public.cast::<u8>(),length as usize)},bytes);
        let lease=mapping(base+PAGE,1,R).unwrap();assert!(lease.private_origin);let pointer=lease.address;
        let error=prepare_initial(base,&vec![0x88;length as usize],&protections,&[base+4]).unwrap().install(|_|Err(std::io::Error::from_raw_os_error(libc::ENOMEM))).unwrap_err();
        assert_eq!(error.raw_os_error(),Some(libc::ENOMEM));assert_eq!(mapping(base+PAGE,1,R).unwrap().address,pointer);assert!(!intent(base,X));
        let generation=epoch();prepare_initial(base,&vec![0x77;length as usize],&protections,&[base+4]).unwrap().install(|publish|publish()).unwrap();replace(base,length,generation);
        assert!(intent(base,X));assert!(!intent(base+PAGE,X));assert_eq!(unsafe{*(pointer as*const u8)},0x37);
        forget(base,length);unsafe{libc::munmap(public,length as usize);}drop(lease);
    }
}
