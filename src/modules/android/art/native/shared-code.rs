//! Executable MAP_SHARED views keep original bytes in their actual shared pager.
//! Every executable page stays NX: an independent writer can change its sites.
use std::io;
use super::{mutable_memory as memory,vmmap};
const PAGE:u64=16384;const X:u64=4;const MAP_TYPE:u64=15;const ANON:u64=0x20;
pub(crate) fn enabled()->bool{super::mutable_code::available()}
fn linux(error:io::Error)->i64{-(crate::errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64)}
pub(crate) fn genuine(region:&vmmap::Region)->bool{
    region.shared&&!super::copies::find(region.start).is_some_and(|(copy,offset)|copy.end>=region.end&&region.file.as_ref().is_some_and(|(_,dev,ino)|copy.dev==*dev&&copy.ino==*ino&&offset==region.offset)
    )
}
pub(crate) fn shared(start:u64,length:u64)->bool{
    let Some(end)=start.checked_add(length)else{return false;};let mut cursor=start;
    for region in vmmap::regions(start,end){if region.start!=cursor||!genuine(&region){return false;}cursor=region.end;}cursor==end
}
// Snapshot the existing owner and physical view in one VM/mapping epoch.
// Memory-entry ports keep the original shared object alive after this scope.
pub(crate) struct ForkGuard{_transaction:std::sync::MutexGuard<'static,()>,_epoch:super::verity_pager::VmMutation}
pub(crate) fn fork_guard()->Option<ForkGuard>{
    if !enabled()||!memory::has_mappings(){return None;}
    let epoch=super::verity_pager::mutation();let transaction=memory::transaction();Some(ForkGuard{_transaction:transaction,_epoch:epoch})
}
pub(crate) fn fork_end(start:u64,end:u64)->u64{
    let Some((lo,hi,_))=memory::ranges(start,end-start).first().copied()else{return end;};if lo==start{hi}else{lo}
}
pub(crate) fn fork_source(start:u64,length:u64)->Option<memory::Lease>{
    if !shared(start,length){return None;}memory::mapping(start,length,0)
}
pub(crate) fn enroll(start:u64,length:u64,protection:i32)->io::Result<()>{
    let prepared=memory::prepare_shared(start,length,protection)?;
    prepared.install(|publish|super::thread::with_guest_threads_quiescent(publish).map_err(io::Error::from_raw_os_error)?)
}
pub(crate) fn mmap(a:[u64;6],normal:impl FnOnce([u64;6])->i64)->Option<i64>{
    if !enabled()||!matches!(a[3]&MAP_TYPE,1|3)||a[2]&X==0{return None;}
    let fixed=a[3]&0x10!=0;let noreplace=a[3]&0x100000!=0;
    if a[1]==0||a[5]%PAGE!=0||(fixed||noreplace)&&a[0]%PAGE!=0{return Some(-(crate::errno::EINVAL as i64));}
    let Some(length)=a[1].checked_add(PAGE-1).and_then(|length|length.checked_div(PAGE)).and_then(|length|length.checked_mul(PAGE))else{return Some(-(crate::errno::ENOMEM as i64));};
    if (fixed||noreplace)&&a[0].checked_add(length).is_none(){return Some(-(crate::errno::ENOMEM as i64));}
    if a[3]&ANON==0{if let Err(error)=super::memfd::check_shared_execution(a[4]as i32){return Some(error);}}
    let mut physical=a;physical[2]&=!X;
    if fixed||noreplace{physical[0]=0;physical[3]&=!(0x10|0x100000);}
    // Requested WRITE remains in the original mmap admission. Only native
    // execute is removed; the original shared object and pager are unchanged.
    let result=normal(physical);if result<0{return Some(result);}
    let source=result as u64;let target=if fixed||noreplace{a[0]}else{source};
    let cache=match super::fuse_cache::remap_owner(source,length){Ok(owner)=>owner,Err(error)=>{super::window::unmap(source,length);return Some(-(error as i64));}};
    let result=(||->io::Result<()>{
        let _transaction=memory::transaction();
        let prepared=if fixed||noreplace{
            let heap_free=(super::window::BASE..super::window::BASE+super::window::SIZE).contains(&target)&&super::window::is_free(target,length);
            memory::prepare_shared_remap(target,source,length,(a[2]&7)as i32,fixed||heap_free)?
        }else{memory::prepare_shared(target,length,(a[2]&7)as i32)?};
        if fixed||noreplace{super::fuse_cache::flush_range(target,length).map_err(io::Error::from_raw_os_error)?;}
        prepared.install(|publish|super::thread::with_guest_threads_quiescent(publish).map_err(io::Error::from_raw_os_error)?)
    })();
    if result.is_ok(){
        if source!=target{super::copies::forget(target,target+length);super::fuse_cache::forget(target,length);if let Some(cache)=cache{super::fuse_cache::remapped(cache,source,length,target,length,false);}}
    }
    if result.is_err(){super::window::unmap(source,length);super::copies::forget(source,source+length);super::fuse_cache::forget(source,length);}
    else if source!=target{
        // A fixed target may overlap the temporary guest range. Only retire
        // its portions outside the successfully published destination.
        for(lo,hi)in[(source,(source+length).min(target)),(source.max(target+length),source+length)]{if lo<hi{super::window::unmap(lo,hi-lo);super::copies::forget(lo,hi);super::fuse_cache::forget(lo,hi-lo);}}
    }
    Some(match result{Ok(())=>target as i64,Err(error)=>linux(error)})
}
pub(crate) fn mprotect(a:[u64;6],normal:impl Fn([u64;6])->i64)->Option<i64>{
    if !enabled()||a[2]&X==0||a[0]%PAGE!=0||a[1]==0{return None;}
    let length=a[1].checked_add(PAGE-1)?.checked_div(PAGE)?.checked_mul(PAGE)?;let end=a[0].checked_add(length)?;
    if super::window::touches_reserved(a[0],end){return None;}
    let regions:Vec<_>=vmmap::regions(a[0],end).collect();if !regions.iter().any(genuine){return None;}
    let mut cursor=a[0];
    for region in regions{
        if cursor<region.start{let result=normal([cursor,region.start-cursor,a[2],0,0,0]);if result<0{return Some(result);}}
        let lo=region.start.max(a[0]);let hi=region.end.min(end);
        if genuine(&region){
            let result={let _transaction=memory::transaction();enroll(lo,hi-lo,(a[2]&7)as i32)};
            if let Err(error)=result{return Some(linux(error));}
        }else{let result=normal([lo,hi-lo,a[2],0,0,0]);if result<0{return Some(result);}}
        cursor=hi;
    }
    if cursor<end{let result=normal([cursor,end-cursor,a[2],0,0,0]);if result<0{return Some(result);}}
    Some(0)
}
