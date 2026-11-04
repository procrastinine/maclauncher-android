// Original bytes have a read-only, non-executable public view. Only this owner
// retains its writable alias. The caller holds the process VM epoch before
// entering a transaction; no lock is retained across a guest or native syscall.
use std::{collections::BTreeMap,sync::{Arc,Mutex,MutexGuard,atomic::{AtomicBool,AtomicPtr,AtomicUsize,AtomicU64,Ordering}}};
use crate::patch::vm;
const PAGE:u64=16384;
// Keep retained source aliases in their own anonymous VM objects. Darwin may
// otherwise coalesce an adjacent ordinary heap/stack mapping into the staging
// object before its alias is retained. This tag is never an admission hint.
const SOURCE_BACKING_TAG:i32=(245u32<<24)as i32;
const R:i32=libc::PROT_READ;const W:i32=libc::PROT_WRITE;const X:i32=libc::PROT_EXEC;
static TRANSACTIONS:Mutex<()>=Mutex::new(());
pub(crate) fn transaction()->MutexGuard<'static,()>{TRANSACTIONS.lock().unwrap_or_else(|error|error.into_inner())}
struct Backing {address:u64,length:u64,private_origin:AtomicBool}
impl Backing{
    fn new(address:u64,length:u64,private:bool)->Self{Self{address,length,private_origin:AtomicBool::new(private)}}
    fn shared_or_unknown(&self){self.private_origin.store(false,Ordering::Release);}
}
impl Drop for Backing{fn drop(&mut self){unsafe{vm::mach_vm_deallocate(vm::task(),self.address,self.length);}}}
#[derive(Clone)]
struct Region {end:u64,offset:u64,protection:i32,generation:u64,backing:Arc<Backing>}
#[derive(Clone,Copy)]
struct Bounds {start:u64,end:u64,protection:i32}
struct Snapshot {ranges:Box<[Bounds]>}
#[derive(Default)]
struct Owner {ranges:BTreeMap<u64,Region>}
static NEXT:AtomicU64=AtomicU64::new(1);
static LIVE:AtomicBool=AtomicBool::new(false);
pub(crate) fn has_mappings()->bool{LIVE.load(Ordering::Acquire)}
pub(crate) fn epoch()->u64{NEXT.load(Ordering::Acquire).saturating_sub(1)}
static OWNER:Mutex<Owner>=Mutex::new(Owner{ranges:BTreeMap::new()});
static SNAPSHOT:AtomicPtr<Snapshot>=AtomicPtr::new(std::ptr::null_mut());
static READERS:AtomicUsize=AtomicUsize::new(0);
fn snapshot(owner:&Owner)->Box<Snapshot>{Box::new(Snapshot{ranges:owner.ranges.iter().map(|(&start,region)|Bounds{start,end:region.end,protection:region.protection}).collect()})}
fn exchange(next:Box<Snapshot>)->*mut Snapshot{LIVE.store(!next.ranges.is_empty(),Ordering::Release);SNAPSHOT.swap(Box::into_raw(next),Ordering::SeqCst)}
fn retire(old:*mut Snapshot){while READERS.load(Ordering::SeqCst)!=0{std::thread::yield_now();}if !old.is_null(){unsafe{drop(Box::from_raw(old));}}}
fn publish(owner:&Owner){retire(exchange(snapshot(owner)));}
/// Signal-safe admission only. The safe host continuation rechecks ownership
/// after acquiring the ordinary VM and transaction guards.
pub(crate) fn intent(address:u64,access:i32)->bool {
    if SNAPSHOT.load(Ordering::SeqCst).is_null(){return false;}
    READERS.fetch_add(1,Ordering::SeqCst);let ptr=SNAPSHOT.load(Ordering::SeqCst);
    let found=if ptr.is_null(){false}else{let ranges=&unsafe{&*ptr}.ranges;let at=ranges.partition_point(|range|range.start<=address);at>0&&address<ranges[at-1].end&&ranges[at-1].protection&access==access};
    READERS.fetch_sub(1,Ordering::SeqCst);found
}
fn remove(owner:&mut Owner,start:u64,length:u64){
    let end=start.saturating_add(length);
    let starts:Vec<_>=owner.ranges.range(..end).filter(|(at,region)|**at<end&&region.end>start).map(|(&at,_)|at).collect();
    for at in starts{let region=owner.ranges.remove(&at).unwrap();if at<start{owner.ranges.insert(at,Region{end:start,..region.clone()});}if region.end>end{owner.ranges.insert(end,Region{offset:region.offset+end-at,..region});}}
}
pub(crate) struct Prepared {start:u64,length:u64,protection:i32,backing:Arc<Backing>,staging:u64}
impl Drop for Prepared{fn drop(&mut self){unsafe{vm::mach_vm_deallocate(vm::task(),self.staging,self.length);}}}
/// All allocation and original-word validation precedes public VM replacement.
/// The supplied bytes are already restored from a leased mapping generation.
pub(crate) fn prepare(start:u64,bytes:&[u8],protection:i32)->std::io::Result<Prepared>{
    let length=bytes.len()as u64;
    if length==0||start%PAGE!=0||length%PAGE!=0||start.checked_add(length).is_none(){return Err(std::io::Error::from_raw_os_error(libc::EINVAL));}
    let address=unsafe{libc::mmap(std::ptr::null_mut(),bytes.len(),R|W,libc::MAP_ANON|libc::MAP_PRIVATE,SOURCE_BACKING_TAG,0)};
    if address==libc::MAP_FAILED{return Err(std::io::Error::last_os_error());}
    unsafe{std::ptr::copy_nonoverlapping(bytes.as_ptr(),address.cast::<u8>(),bytes.len());}
    let Some(alias)=vm::alias(address as u64,length,None,R|W)else{unsafe{libc::munmap(address,bytes.len());}return Err(std::io::Error::last_os_error());};
    let prepared=Prepared{start,length,protection,backing:Arc::new(Backing::new(alias,length,true)),staging:address as u64};
    if unsafe{libc::mprotect(address,bytes.len(),protection&R)}!=0{return Err(std::io::Error::last_os_error());}
    Ok(prepared)
}
impl Prepared {
    /// The caller's original thread owner suspends guest execution only around
    /// this already-prepared remap. No allocation or new lock occurs inside it.
    pub(crate) fn install(self,suspend:impl FnOnce(&mut dyn FnMut()->std::io::Result<()>)->std::io::Result<()>)->std::io::Result<()> {
        let mut owner=OWNER.lock().unwrap_or_else(|error|error.into_inner());
        let previous=owner.ranges.clone();remove(&mut owner,self.start,self.length);
        owner.ranges.insert(self.start,Region{end:self.start+self.length,offset:0,protection:self.protection,generation:NEXT.fetch_add(1,Ordering::AcqRel),backing:self.backing.clone()});
        // Publish admission first while the transaction holds the safe owner.
        // Any admitted thread waits and rechecks the committed state below.
        let old=exchange(snapshot(&owner));
        let mut remap=||{
            let(mut at,mut current,mut maximum)=(self.start,0,0);
            let kr=unsafe{vm::mach_vm_remap(vm::task(),&mut at,self.length,0,vm::VM_FLAGS_FIXED|vm::VM_FLAGS_OVERWRITE,vm::task(),self.staging,0,&mut current,&mut maximum,vm::VM_INHERIT_COPY)};
            if kr!=0{return Err(std::io::Error::from_raw_os_error(libc::ENOMEM));}
            // The source is already RO+NX, with an RW maximum for its private
            // alias. Mach remap preserves those protections atomically.
            assert_eq!(at,self.start);assert_eq!(current,self.protection&R);Ok(())
        };
        if let Err(error)=suspend(&mut remap){owner.ranges=previous;LIVE.store(!owner.ranges.is_empty(),Ordering::Release);let failed=SNAPSHOT.swap(old,Ordering::SeqCst);retire(failed);return Err(error);}
        retire(old);Ok(())
    }
}
/// Pointer lifetime is tied to a mapping lease, never a pathname or cache hint.
pub(crate) struct Lease {pub start:u64,pub length:u64,pub address:u64,pub protection:i32,pub private_origin:bool,_backing:Arc<Backing>}
pub(crate) fn mapping(address:u64,length:u64,access:i32)->Option<Lease>{
    let owner=OWNER.lock().unwrap_or_else(|error|error.into_inner());let(&start,region)=owner.ranges.range(..=address).next_back()?;
    if address>=region.end||length>region.end-address||region.protection&access!=access{return None;}
    Some(Lease{start,length:region.end-start,address:region.backing.address+region.offset,protection:region.protection,private_origin:region.backing.private_origin.load(Ordering::Acquire),_backing:region.backing.clone()})
}
pub(crate) fn covers(address:u64,length:u64)->bool{mapping(address,length,0).is_some()}
pub(crate) fn protection(address:u64,actual:i32)->i32{mapping(address,1,0).map_or(actual,|lease|lease.protection)}
/// The caller retains the original VM epoch and transaction guard. This is the
/// same source view used by instruction fetch and native atomic operations.
pub(crate) fn write(address:u64,bytes:&[u8])->Result<(),crate::errno::Errno>{
    let lease=mapping(address,bytes.len()as u64,W).ok_or(crate::errno::EFAULT)?;
    unsafe{std::ptr::copy(bytes.as_ptr(),(lease.address+address-lease.start)as*mut u8,bytes.len());}Ok(())
}
pub(crate) fn forget(start:u64,length:u64){let mut owner=OWNER.lock().unwrap_or_else(|error|error.into_inner());remove(&mut owner,start,length);publish(&owner);}
pub(crate) fn reset(){let mut owner=OWNER.lock().unwrap_or_else(|error|error.into_inner());owner.ranges.clear();publish(&owner);}
/// Keep owned original-code views NX even after a later RX request. Native
/// execution cannot observe a half-translated generation between writes.
pub(crate) fn protect(start:u64,length:u64,protection:i32)->Option<std::io::Result<()>> {
    let mut owner=OWNER.lock().unwrap_or_else(|error|error.into_inner());let end=start.checked_add(length)?;let mut cursor=start;
    while cursor<end{let(&at,region)=owner.ranges.range(..=cursor).next_back()?;if at>cursor||region.end<=cursor{return None;}cursor=region.end.min(end);}
    if unsafe{libc::mprotect(start as*mut _,length as usize,protection&R)}!=0{return Some(Err(std::io::Error::last_os_error()));}
    let pieces:Vec<_>=owner.ranges.range(..end).filter(|(at,region)|**at<end&&region.end>start).map(|(&at,region)|(at.max(start),region.end.min(end),region.offset+at.max(start)-at,region.backing.clone())).collect();
    remove(&mut owner,start,length);for(at,end,offset,backing)in pieces{owner.ranges.insert(at,Region{end,offset,protection,generation:NEXT.fetch_add(1,Ordering::AcqRel),backing});}publish(&owner);Some(Ok(()))
}
/// Mach remap shares the actual object already retained by the source lease.
/// No descriptor/alias allocation occurs after the public move commits. The
/// caller retires the old range only after its original unmap/discard succeeds.
pub(crate) fn relocated(old:u64,new:u64,length:u64)->std::io::Result<()> {
    let mut owner=OWNER.lock().unwrap_or_else(|error|error.into_inner());
    let pieces:Vec<_>=owner.ranges.range(..old.saturating_add(length)).filter(|(at,region)|**at<old+length&&region.end>old).map(|(&at,region)|(at.max(old),region.end.min(old+length),region.offset+at.max(old)-at,region.clone())).collect();
    remove(&mut owner,new,length);
    for(at,end,offset,region)in pieces{region.backing.shared_or_unknown();let start=new+at-old;owner.ranges.insert(start,Region{end:start+end-at,offset,generation:NEXT.fetch_add(1,Ordering::AcqRel),..region});}
    publish(&owner);Ok(())
}
pub(crate) fn replace(start:u64,length:u64,epoch:u64){
    let mut owner=OWNER.lock().unwrap_or_else(|error|error.into_inner());let end=start.saturating_add(length);
    let pieces:Vec<_>=owner.ranges.range(..end).filter(|(at,region)|**at<end&&region.end>start&&region.generation<=epoch).map(|(&at,region)|(at.max(start),region.end.min(end))).collect();
    for(at,end)in pieces{remove(&mut owner,at,end-at);}publish(&owner);
}
pub(crate) fn overlaps(start:u64,length:u64)->bool{!ranges(start,length).is_empty()}
/// Proc metadata is virtual. All actual VM/fork owners continue to use Darwin
/// regions directly, preserving the RO+NX public-view contract.
pub(crate) fn proc_regions(lo:u64,hi:u64)->Vec<super::vmmap::Region>{
    let mut result=Vec::new();
    for region in super::vmmap::regions(lo,hi){
        let owned=ranges(region.start,region.end-region.start);let mut cuts=vec![region.start,region.end];
        for(start,end,_)in owned{cuts.push(start);cuts.push(end);}cuts.sort_unstable();cuts.dedup();
        for piece in cuts.windows(2){let(start,end)=(piece[0],piece[1]);result.push(super::vmmap::Region{start,end,prot:protection(start,region.prot as i32)as u32,shared:region.shared,offset:region.offset+start-region.start,file:region.file.clone(),usage:region.usage});}
    }result
}
pub(crate) fn ranges(start:u64,length:u64)->Vec<(u64,u64,i32)>{
    OWNER.lock().unwrap_or_else(|error|error.into_inner()).ranges.range(..start.saturating_add(length)).filter(|(at,region)|**at<start+length&&region.end>start).map(|(&at,region)|(at.max(start),region.end.min(start+length),region.protection)).collect()
}
pub(crate) fn save(writer:&mut super::fork_state::Writer){
    let owner=OWNER.lock().unwrap_or_else(|error|error.into_inner());for region in owner.ranges.values(){region.backing.shared_or_unknown();}writer.seq(owner.ranges.iter(),|writer,(&start,region)|{writer.u64(start);writer.u64(region.end);writer.i32(region.protection);});
}
pub(crate) fn restore(reader:&mut super::fork_state::Reader){
    let records=reader.seq(|reader|(reader.u64(),reader.u64(),reader.i32()));let mut owner=OWNER.lock().unwrap_or_else(|error|error.into_inner());owner.ranges.clear();
    for(start,end,protection)in records{let length=end-start;assert!(start%PAGE==0&&length!=0&&length%PAGE==0,"original mutable mapping bounds");let alias=vm::alias(start,length,None,R|W).expect("original mutable fork alias");owner.ranges.insert(start,Region{end,offset:0,protection,generation:NEXT.fetch_add(1,Ordering::AcqRel),backing:Arc::new(Backing::new(alias,length,false))});}publish(&owner);
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn original_views_keep_exact_lifetime_and_refused_publications(){
        let _transaction=transaction();reset();
        let length=2*PAGE;let public=unsafe{libc::mmap(std::ptr::null_mut(),length as usize,R|W,libc::MAP_PRIVATE|libc::MAP_ANON,-1,0)};
        assert_ne!(public,libc::MAP_FAILED);let base=public as u64;
        let bytes=vec![0x37;length as usize];prepare(base,&bytes,R|W|X).unwrap().install(|publish|publish()).unwrap();
        assert_eq!(vm::region(base).unwrap().2,R);assert!(intent(base,X));assert!(intent(base,W));assert!(!intent(base+length,X));
        assert_eq!(unsafe{std::slice::from_raw_parts(public.cast::<u8>(),length as usize)},bytes);
        write(base+PAGE-2,&[1,2,3,4]).unwrap();assert_eq!(unsafe{std::slice::from_raw_parts((base+PAGE-2)as*const u8,4)},[1,2,3,4]);
        let old=mapping(base,1,W).unwrap();let pointer=old.address;let denied=prepare(base,&vec![0x88;length as usize],R|W|X).unwrap().install(|_|Err(std::io::Error::from_raw_os_error(libc::ENOMEM)));
        assert_eq!(denied.unwrap_err().raw_os_error(),Some(libc::ENOMEM));assert_eq!(mapping(base,1,W).unwrap().address,pointer);assert_eq!(unsafe{*(base as*const u8)},0x37);
        protect(base,PAGE,R|X).unwrap().unwrap();assert!(intent(base,X));assert!(!intent(base,W));assert!(intent(base+PAGE,W));assert!(write(base,&[9]).is_err());
        assert_eq!(vm::region(base).unwrap().2,R);write(base+PAGE,&[11]).unwrap();
        // Actual shared Mach move/alias and replacement generations retain
        // exact old objects; metadata never recreates a writable public view.
        let alias=vm::alias(base,length,None,R).unwrap();relocated(base,alias,length).unwrap();
        assert!(intent(alias,X));assert_eq!(vm::region(alias).unwrap().2,R);
        protect(alias,length,R|W|X).unwrap().unwrap();write(alias+PAGE,&[29]).unwrap();
        assert_eq!(unsafe{*((base+PAGE)as*const u8)},29);
        let before=epoch();let moved=mapping(alias,1,R).unwrap();
        prepare(base,&vec![0;length as usize],R|W|X).unwrap().install(|publish|publish()).unwrap();
        replace(base,length,before);assert!(intent(base,X));assert_eq!(unsafe{*(base as*const u8)},0);
        assert_eq!(unsafe{*(alias as*const u8)},0x37);assert_eq!(unsafe{*(moved.address as*const u8)},0x37);
        forget(alias,length);unsafe{libc::munmap(alias as*mut _,length as usize);}drop(moved);
        forget(base,length);assert!(!intent(base,X));assert!(mapping(base,1,0).is_none());unsafe{libc::munmap(public,length as usize);}
        // An in-flight instruction still owns its exact source object even when
        // public registration was retired; the next instruction must re-admit.
        assert_eq!(unsafe{*(pointer as*const u8)},0x37);drop(old);
    }
}

#[cfg(test)]
mod private_origin_tests{
    use super::*;
    #[test]
    fn private_hint_downgrades_for_aliases_and_fork_without_changing_permissions(){
        let _epoch=super::super::verity_pager::mutation();let _transaction=transaction();reset();
        let raw=unsafe{libc::mmap(std::ptr::null_mut(),PAGE as usize,R|W,libc::MAP_PRIVATE|libc::MAP_ANON,-1,0)};assert_ne!(raw,libc::MAP_FAILED);let base=raw as u64;
        prepare(base,&vec![0x37;PAGE as usize],R|X).unwrap().install(|publish|publish()).unwrap();
        assert!(mapping(base,1,R).unwrap().private_origin);assert_eq!(vm::region(base).unwrap().2,R);
        let alias=vm::alias(base,PAGE,None,R).unwrap();relocated(base,alias,PAGE).unwrap();
        assert!(!mapping(base,1,R).unwrap().private_origin);assert!(!mapping(alias,1,R).unwrap().private_origin);
        assert_eq!(vm::region(alias).unwrap().2,R);forget(alias,PAGE);unsafe{libc::munmap(alias as*mut _,PAGE as usize);}
        prepare(base,&vec![0x48;PAGE as usize],R|X).unwrap().install(|publish|publish()).unwrap();assert!(mapping(base,1,R).unwrap().private_origin);
        let mut writer=super::super::fork_state::Writer::default();save(&mut writer);assert!(!mapping(base,1,R).unwrap().private_origin,"parent loses hint before fork handoff");
        let bytes=writer.into_bytes();let mut reader=super::super::fork_state::Reader::new(&bytes);restore(&mut reader);
        assert!(!mapping(base,1,R).unwrap().private_origin,"deserialized backing requires original classification");
        assert_eq!(unsafe{*(base as*const u8)},0x48);assert_eq!(vm::region(base).unwrap().2,R);assert!(intent(base,X));assert!(!intent(base,W));
        forget(base,PAGE);unsafe{libc::munmap(raw,PAGE as usize);}
    }
}
