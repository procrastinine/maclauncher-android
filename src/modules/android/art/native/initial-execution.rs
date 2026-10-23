// Sparse original-instruction execution. The public data view always contains
// the original bytes; only pages with actual rewrite sites become owned NX.
use std::io;
const PAGE:u64=16384;
pub(crate) fn enabled()->bool{super::mutable_code::available()}
fn error()->io::Error{io::Error::from_raw_os_error(libc::EINVAL)}
pub(crate) fn file_sites(base:u64,length:u64,offset:u64,source:&crate::xrt::ExecSource)->io::Result<Vec<u64>>{
    if base.checked_add(length).is_none()||offset.checked_add(length).is_none(){return Err(error());}
    match source {
        crate::xrt::ExecSource::LoadTime(Some(sites))=>Ok(sites.sites.iter().filter(|&&(at,_,_)|at>=offset&&at.checked_add(4).is_some_and(|end|end<=offset+length)).map(|&(at,_,_)|base+at-offset).collect()),
        crate::xrt::ExecSource::Shared=>Ok(Vec::new()),
        crate::xrt::ExecSource::LoadTime(None)=>{
            let bytes=unsafe{std::slice::from_raw_parts(base as*const u8,length as usize)};
            scan_sites(base,bytes)
        }
    }
}
pub(crate) fn scan_sites(base:u64,bytes:&[u8])->io::Result<Vec<u64>>{
    // Reuse the existing complete surveyed instruction/data analysis when an
    // executable mapping has no authenticated ELF site record.
    let prepared=crate::patch::prepare_region_at(base,bytes,true,"original-instruction-sites")?;
    Ok(prepared.words.iter().map(|&(address,_)|address).collect())
}
pub(crate) fn publish(base:u64,bytes:&[u8],protections:&[i32],sites:&[u64])->io::Result<()> {
    let _transaction=super::mutable_memory::transaction();
    let prepared=super::mutable_memory::prepare_initial(base,bytes,protections,sites)?;
    prepared.install(|publish|super::thread::with_guest_threads_quiescent(publish).map_err(io::Error::from_raw_os_error)?)?;
    crate::patch::forget_far(base,bytes.len()as u64);Ok(())
}
pub(crate) fn uniform(base:u64,bytes:&[u8],protection:i32,sites:&[u64])->io::Result<()> {
    if bytes.len()%PAGE as usize!=0{return Err(error());}
    publish(base,bytes,&vec![protection;bytes.len()/PAGE as usize],sites)
}
pub(crate) fn file_copy(base:u64,length:u64,fd:i32,offset:u64,source:&crate::xrt::ExecSource,protection:i32)->io::Result<()> {
    let sites=file_sites(base,length,offset,source)?;
    if sites.is_empty(){return Ok(());}
    let bytes=unsafe{std::slice::from_raw_parts(base as*const u8,length as usize)}.to_vec();
    uniform(base,&bytes,protection,&sites)?;
    let _=fd;Ok(())
}
/// Preserve owned NX pages when an enclosing segment receives its final
/// virtual permissions. Unowned pages retain the original Darwin operation.
pub(crate) fn protect(base:u64,length:u64,protection:i32)->io::Result<()> {
    let end=base.checked_add(length).ok_or_else(error)?;let mut cursor=base;
    for(lo,hi,_)in super::mutable_memory::ranges(base,length){
        if cursor<lo&&unsafe{libc::mprotect(cursor as*mut _,(lo-cursor)as usize,protection)}!=0{return Err(io::Error::last_os_error());}
        super::mutable_memory::protect(lo,hi-lo,protection).ok_or_else(error)??;cursor=hi;
    }
    if cursor<end&&unsafe{libc::mprotect(cursor as*mut _,(end-cursor)as usize,protection)}!=0{return Err(io::Error::last_os_error());}Ok(())
}
/// Lazily retain authenticated ELF sites alongside an original fs-verity
/// mapping. No derivative FD or header is exposed to the guest.
pub(crate) fn verified_pages(source:&super::verity_pager::Source,offset:u64)->io::Result<Option<std::sync::Arc<super::verity_pager::RewritePages>>>{
    use std::os::fd::AsFd;
    let original=std::sync::Arc::new(super::verified_source::VerifiedSource::from_descriptor(source.data.as_fd(),source.description.clone()).map_err(io::Error::from_raw_os_error)?);
    let mut header=[0;20];if original.len().map_err(io::Error::from_raw_os_error)?<header.len()as u64{return Ok(None);}
    original.read_exact_at(&mut header,0).map_err(io::Error::from_raw_os_error)?;
    let host=std::path::Path::new("");
    let plan=if crate::xlate::elf::is_aarch64_elf(&header){
        let length=original.len().map_err(io::Error::from_raw_os_error)?;
        crate::xrt::verified::prepare_verified_rewrite(original,host,&(0..length)).map_err(io::Error::from_raw_os_error)?
    }else if header[..4]==*b"PK\x03\x04"{
        crate::xrt::verified::prepare_verified_member_rewrite(original,host,offset).map_err(io::Error::from_raw_os_error)?.rewrite
    }else{return Ok(None);};
    let end=plan.binding.source_length;
    super::verity_pager::RewritePages::prepare(plan,0,0,end).map(|(pages,_)|Some(pages))
}
pub(crate) fn region_copy(base:u64,length:u64,protection:i32)->io::Result<()> {
    use std::os::fd::AsRawFd;
    let end=base.checked_add(length).ok_or_else(error)?;
    // Physical sharing used for a logical private ELF mapping is identified
    // by its existing copy record. A genuine shared source stays separate.
    for region in super::vmmap::regions(base,end){
        if region.shared&&!super::copies::find(region.start).is_some_and(|(copy,offset)|copy.end>=region.end&&region.file.as_ref().is_some_and(|(_,dev,ino)|copy.dev==*dev&&copy.ino==*ino&&offset==region.offset)){
            return Err(io::Error::from_raw_os_error(libc::ENOEXEC));
        }
    }
    let bytes=unsafe{std::slice::from_raw_parts(base as*const u8,length as usize)}.to_vec();let mut sites=Vec::new();let mut cursor=base;
    for(start,copy)in super::copies::file_copies(base,end){
        let begin=start.max(base);let stop=copy.end.min(end);
        if cursor<begin{sites.extend(scan_sites(cursor,&bytes[(cursor-base)as usize..(begin-base)as usize])?);}
        let descriptor=copy.source.as_ref().ok_or_else(error)?.as_raw_fd();let offset=copy.offset+begin-start;
        sites.extend(file_sites(begin,stop-begin,offset,&crate::xrt::exec_source(descriptor,offset))?);cursor=stop;
    }
    if cursor<end{sites.extend(scan_sites(cursor,&bytes[(cursor-base)as usize..])?);}
    if !sites.is_empty(){uniform(base,&bytes,protection,&sites)?;}Ok(())
}
/// The caller holds the VM epoch and mutable transaction. Only a unique
/// anonymous, non-executable Darwin object can omit a source-write barrier.
pub(crate) fn private_data(base:u64,length:u64)->bool{PrivateDataCache::default().eligible(base,length)}
#[derive(Default)]
pub(crate) struct PrivateDataCache{regions:Vec<super::vmmap::Region>}
impl PrivateDataCache{
    pub(crate) fn clear(&mut self){self.regions.clear();}
    pub(crate) fn eligible(&mut self,base:u64,length:u64)->bool{
        let Some(end)=base.checked_add(length)else{return false;};
        if length==0||super::mutable_memory::overlaps(base,length){return false;}
        if let Some(region)=self.regions.iter().find(|region|region.start<=base&&region.end>=end){return private_region(region);}
        let Some(region)=super::vmmap::region_at(base)else{return false;};
        let eligible=region.start<=base&&region.end>=end&&private_region(&region);
        if progress_enabled()||vm_sample_enabled(){
            let query=PRIVATE_QUERIES.fetch_add(1,std::sync::atomic::Ordering::Relaxed)+1;
            if eligible{PRIVATE_ELIGIBLE.fetch_add(1,std::sync::atomic::Ordering::Relaxed);}
            if !eligible&&region.prot&3==3&&vm_sample_enabled(){sample_region(base,length,&region,query);}
        }
        self.regions.push(region);eligible
    }
}
fn private_region(region:&super::vmmap::Region)->bool{
    region.prot==3&&!region.shared&&region.file.is_none()&&region.usage.share_mode==super::vmmap::SM_PRIVATE&&region.usage.ref_count==1
}
fn vm_sample_enabled()->bool{
    static ENABLED:std::sync::OnceLock<bool>=std::sync::OnceLock::new();
    *ENABLED.get_or_init(||std::env::var_os("MACLAUNCHER_ART_PRECISE_VM_SAMPLE").is_some_and(|value|value=="1"))
}
fn sample_region(base:u64,length:u64,region:&super::vmmap::Region,query:u64){
    static SEEN:std::sync::Mutex<Vec<u64>>=std::sync::Mutex::new(Vec::new());
    static FULL:std::sync::atomic::AtomicBool=std::sync::atomic::AtomicBool::new(false);
    let first=if FULL.load(std::sync::atomic::Ordering::Relaxed){false}else{
        let mut seen=SEEN.lock().unwrap();
        if seen.contains(&region.start){false}else{seen.push(region.start);if seen.len()==16{FULL.store(true,std::sync::atomic::Ordering::Relaxed);}true}
    };
    if first||query.is_power_of_two(){
        eprintln!("[original-instruction] vm-candidate query={query} requested={base:#x}+{length:#x} entry={:#x}..{:#x} prot={} shared={} mode={} refs={} offset={:#x} file={:?}",region.start,region.end,region.prot,region.shared,region.usage.share_mode,region.usage.ref_count,region.offset,region.file);
    }
}
pub(crate) fn report_stop(kind:u32,detail:u32,address:u64,error:i32,pc:u64,instruction:u32){
    if std::env::var_os("MACLAUNCHER_ART_PRECISE_FAULT").is_some_and(|value|value=="1"){
        eprintln!("[original-instruction] precise stop kind={kind} detail={detail} error={error} address={address:#x} pc={pc:#x} word={instruction:08x} ({})",crate::diag::describe(pc));
    }
}
pub(crate) fn budget()->u32{
    static SINGLE:std::sync::OnceLock<bool>=std::sync::OnceLock::new();
    if *SINGLE.get_or_init(||std::env::var_os("MACLAUNCHER_ART_PRECISE_SINGLE_STEP").is_some_and(|value|value=="1")){1}else{1024}
}
static PRIVATE_QUERIES:std::sync::atomic::AtomicU64=std::sync::atomic::AtomicU64::new(0);
static PRIVATE_ELIGIBLE:std::sync::atomic::AtomicU64=std::sync::atomic::AtomicU64::new(0);
fn progress_enabled()->bool{
    static ENABLED:std::sync::OnceLock<bool>=std::sync::OnceLock::new();
    *ENABLED.get_or_init(||std::env::var_os("MACLAUNCHER_ART_PRECISE_PROGRESS").is_some_and(|value|value=="1"))
}
pub(crate) fn progress(before:u64,after:u64,instruction:u32,x:[u64;4]){
    if !progress_enabled(){return;}
    static COUNT:std::sync::atomic::AtomicU64=std::sync::atomic::AtomicU64::new(0);
    let count=COUNT.fetch_add(1,std::sync::atomic::Ordering::Relaxed)+1;
    if count.is_power_of_two(){eprintln!("[original-instruction] progress calls={count} from={before:#x} next={after:#x} last_word={instruction:08x} x0={:#x} x1={:#x} x2={:#x} x3={:#x}",x[0],x[1],x[2],x[3]);eprintln!("[original-instruction] private data queries={} eligible={}",PRIVATE_QUERIES.load(std::sync::atomic::Ordering::Relaxed),PRIVATE_ELIGIBLE.load(std::sync::atomic::Ordering::Relaxed));}
}

#[cfg(test)]
mod admission_tests{
    use super::*;
    #[test]
    fn region_queries_are_local_to_the_held_vm_interval(){
        let _epoch=super::super::verity_pager::mutation();
        let _transaction=super::super::mutable_memory::transaction();
        let page=16384usize;
        let raw=unsafe{libc::mmap(std::ptr::null_mut(),3*page,3,libc::MAP_PRIVATE|libc::MAP_ANON,-1,0)};
        assert_ne!(raw,libc::MAP_FAILED);
        assert_eq!(unsafe{libc::mprotect(raw,page,0)},0);
        let base=raw as u64+page as u64;
        unsafe{std::ptr::write_bytes(base as*mut u8,0x37,2*page);}
        let mut cache=PrivateDataCache::default();
        assert!(cache.eligible(base,4096));assert!(cache.eligible(base+4096,4096));
        assert_eq!(cache.regions.len(),1,"same actual object queried once within the VM lease");
        cache.clear();
        let alias=crate::patch::vm::alias(base,page as u64,None,3).unwrap();
        assert!(!cache.eligible(base,4096));assert!(!cache.eligible(base+4096,4096));
        assert_eq!(cache.regions.len(),1,"negative object query also reused within the VM lease");
        cache.clear();
        assert_eq!(unsafe{libc::mprotect(base as*mut _,page,1)},0);
        assert!(!cache.eligible(base,4096));
        assert!(!cache.eligible(base+page as u64-2048,4096),"cache cannot grant across a protection boundary");
        cache.clear();
        unsafe{libc::munmap(alias as*mut _,page);libc::munmap(raw,3*page);}
    }
}
