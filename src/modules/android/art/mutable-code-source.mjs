import fs from 'node:fs';
import path from 'node:path';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';
import {prepareOriginalCode} from './original-code-source.mjs';
import {prepareHostcallMemory} from './hostcall-memory-source.mjs';

/** Keep original mutable text in its own byte and execution owner. */
export function prepareMutableCode(aim){
 prepareOriginalCode(aim);
 const root=path.join(aim,'crates/aim-linux-abi/src');
 const edit=(name,changes)=>{const file=path.join(root,name);let text=fs.readFileSync(file,'utf8');for(const[from,to,count=1]of changes){if(text.split(from).length!==count+1)throw Error('Original mutable owner boundary changed: '+name+' '+from.slice(0,90));text=text.split(from).join(to);}writeChanged(file,text);};
 for(const name of ['mutable-code','mutable-memory','precise-ffi'])copyChanged(path.join(import.meta.dirname,'native',name+'.rs'),path.join(root,'sys',name+'.rs'));
 const build=path.join(aim,'crates/aim-linux-abi/build.rs');let script=fs.readFileSync(build,'utf8');
 const main='fn main() {';if(script.split(main).length!==2)throw Error('Original precise component build boundary changed');
 script=script.replace(main,main+`
    println!("cargo:rustc-check-cfg=cfg(maclauncher_precise)");
    println!("cargo:rerun-if-env-changed=MACLAUNCHER_PRECISE_ARCHIVES");
    if let Some(archives)=env::var_os("MACLAUNCHER_PRECISE_ARCHIVES") {
        for archive in env::split_paths(&archives) {
            println!("cargo:rerun-if-changed={}",archive.display());
            println!("cargo:rustc-link-search=native={}",archive.parent().unwrap().display());
            let name=archive.file_name().unwrap().to_str().unwrap().strip_prefix("lib").unwrap().strip_suffix(".a").unwrap();
            println!("cargo:rustc-link-lib=static={name}");
        }
        println!("cargo:rustc-link-lib=c++");println!("cargo:rustc-cfg=maclauncher_precise");
    }
 `);writeChanged(build,script);
 edit('sys/mod.rs',[
  ['mod jit;','mod jit;\n#[path="mutable-code.rs"]\npub(crate) mod mutable_code;\n#[path="mutable-memory.rs"]\npub(crate) mod mutable_memory;\n#[cfg(maclauncher_precise)]\n#[path="precise-ffi.rs"]\npub(crate) mod precise_ffi;'],
  ['#[cfg(maclauncher_a64)]\npub(crate) use signal::{shadow_deliver,shadow_fault};','#[cfg(any(maclauncher_a64,maclauncher_precise))]\npub(crate) use signal::{shadow_deliver,shadow_fault};'],
 ]);
 edit('sys/signal.rs',[
  ['    let m = unsafe { sigframe::mcontext(uc) };','    let m = unsafe { sigframe::mcontext(uc) };\n    if super::mutable_code::native_fault(hsig,si.si_code,si.si_addr as u64,unsafe{&(*(uc as*const libc::ucontext_t)).uc_sigmask}as*const _ as*const _){return None;}'],
  ['        // A resident MAP_JIT page may fault only to switch its per-thread','        if matches!(hsig,libc::SIGSEGV|libc::SIGBUS)&&super::mutable_code::fault(ctx,m){return None;}\n        // A resident MAP_JIT page may fault only to switch its per-thread'],
  ['#[cfg(maclauncher_a64)]\npub(crate) fn shadow_','#[cfg(any(maclauncher_a64,maclauncher_precise))]\npub(crate) fn shadow_',2],
 ]);
 edit('context.rs',[
  ['pub fn resume_trap() -> ! {','pub fn resume_trap() -> ! {\n    crate::sys::mutable_code::resume_dispatch();\n    resume_native_trap()\n}\n\npub(crate) fn resume_native_trap() -> ! {'],
 ]);
 edit('patch.rs',[
  ['pub fn note_original_words(words:&[(u64,u32,u32)])',`pub fn has_original_words(start:u64,length:u64)->bool{original::overlaps(start,length)}
pub fn is_veneer_pc(pc:u64)->bool{ISLANDS.lock().unwrap_or_else(|error|error.into_inner()).iter().any(|island|pc>=island.rx&&pc-island.rx<island.size)}
pub fn far_continuation(pc:u64,token:u16)->Option<u64>{far::continuation(pc,token)}
pub fn note_original_words(words:&[(u64,u32,u32)])`],
 ]);
 edit('far-veneer.rs',[
  ['fn lookup(pc:u64,token:u16)->Option<u64>{','pub(super) fn continuation(pc:u64,token:u16)->Option<u64>{lookup(pc,token)}\nfn lookup(pc:u64,token:u16)->Option<u64>{'],
 ]);
 edit('loader.rs',[
  ['            map_file_backed(&file, &h, bias, name)?;',`            map_file_backed(&file, &h, bias, name)?;
            // Physical cache pages may be shared; original ELF LOADs retain
            // private mapping semantics when write permission is later added.
            for p in h.loads.iter().filter(|p|p.p_filesz>0){
                let start=page_down(bias+p.p_vaddr);let length=page_up(bias+p.p_vaddr+p.p_filesz)-start;
                crate::sys::note_copy(start,length,&mapped,name,page_down(p.p_offset));
            }`],
 ]);
 edit('sys/copies.rs',[
  ['pub fn moved(old: u64, new: u64, len: u64) {','pub fn moved(old:u64,new:u64,len:u64){relocated(old,new,len,false)}\npub fn aliased(old:u64,new:u64,len:u64){relocated(old,new,len,true)}\nfn relocated(old: u64, new: u64, len: u64, keep:bool) {'],
  ['    forget(old, old + len);','    if !keep{forget(old, old + len);}'],
 ]);
 edit('sys/fork/state.rs',[
  ['    mem::fork_save(w);','    mem::fork_save(w);crate::sys::mutable_memory::save(w);'],
  ['    mem::fork_restore(r);','    mem::fork_restore(r);crate::sys::mutable_memory::restore(r);'],
 ]);
 edit('sys/user_memory.rs',[
  ['        if start > at || stop <= at || protection & required == 0 {','        let protection=super::mutable_memory::protection(at,protection);\n        if start > at || stop <= at || protection & required == 0 {'],
  ['        // SAFETY: source is our live slice; Mach validates the user destination.',`        if super::mutable_memory::covers(at,1){
            let _transaction=super::mutable_memory::transaction();
            let lease=super::mutable_memory::mapping(at,1,libc::PROT_WRITE).ok_or(errno::EFAULT)?;
            let count=count.min((lease.start+lease.length-at)as usize);
            super::mutable_memory::write(at,&bytes[done..done+count])?;done+=count;continue;
        }
        // SAFETY: source is our live slice; Mach validates the user destination.`],
 ]);
 edit('sys/procfs.rs',[
  ['    let mut regions = vmmap::regions(0, u64::MAX).peekable();','    let mut regions = super::mutable_memory::proc_regions().into_iter().peekable();'],
 ]);
 edit('sys/user-output.rs',[
  ['  if protection&7==7&&super::arena::contains(lo,hi-lo){return true;}','  if super::mutable_memory::overlaps(lo,hi-lo)||protection&7==7&&super::arena::contains(lo,hi-lo){return true;}'],
 ]);
 edit('sys/mem.rs',[
  ['            // Translated (or nothing to rewrite): the file itself, shared.','            // Translated (or nothing to rewrite): the file itself, shared.\n            // The original guest request remains MAP_PRIVATE.\n            noted=true;'],
  ['let _epoch=super::verity_pager::mutation();','let _epoch=super::mutable_code::mapping_mutation();',7],
  ['        let _epoch=super::mutable_code::mapping_mutation();\n        let result=mmap_owned(a);','        let _epoch=super::mutable_code::mapping_mutation();let mutable_epoch=super::mutable_memory::epoch();\n        let result=mmap_owned(a);'],
  ['    let _epoch=super::mutable_code::mapping_mutation();\n    if a[3]&MAP_FIXED!=0','    let _epoch=super::mutable_code::mapping_mutation();let mutable_epoch=super::mutable_memory::epoch();\n    if a[3]&MAP_FIXED!=0'],
  ['if result>=0{patch::replace_far(result as u64,page_up(a[1]),far_epoch);}','if result>=0{patch::replace_far(result as u64,page_up(a[1]),far_epoch);super::mutable_memory::replace(result as u64,page_up(a[1]),mutable_epoch);}',2],
  ['    DEFERRED_UNMAPS.with(|d| d.borrow_mut().clear());','    DEFERRED_UNMAPS.with(|d| d.borrow_mut().clear());super::mutable_memory::reset();'],
  ['        patch::forget_far(addr,len);','        patch::forget_far(addr,len);super::mutable_memory::forget(addr,len);'],
  ['            unsafe { libc::munmap(addr as *mut _, len as usize) };','            if unsafe { libc::munmap(addr as *mut _, len as usize) }==0{super::mutable_memory::forget(addr,len);patch::forget_far(addr,len);}'],
  ['pub fn mprotect(a: [u64; 6]) -> i64 {',`pub fn mprotect(a: [u64; 6]) -> i64 {
    if super::mutable_code::available()&&a[2]&PROT_WRITE!=0&&patch::has_original_words(a[0],page_up(a[1])) {
        if let Err(error)=super::verity_pager::prepare_user_range(a[0],page_up(a[1]),super::verity_pager::Access::Read){return -(error as i64);}
    }`],
  ['fn mprotect_owned(a:[u64;6])->i64 {','fn mprotect_owned(a:[u64;6])->i64 {\n    if let Some(result)=mutable_protect(a){return result;}'],
  ['fn discard_private(addr: u64, len: u64) -> i64 {',`fn discard_private(addr:u64,len:u64)->i64{
    let owned=super::mutable_memory::ranges(addr,len);if owned.is_empty(){return discard_private_kernel(addr,len);}
    let mut cursor=addr;
    for(lo,hi,protection)in owned{
        if cursor<lo{let result=discard_private_kernel(cursor,lo-cursor);if result<0{return result;}}
        let result=(||->std::io::Result<()>{
            let _transaction=super::mutable_memory::transaction();
            let bytes=xrt::reload_original_mapping(lo,hi-lo)?;
            super::mutable_memory::prepare(lo,&bytes,protection)?.install(|publish|super::thread::with_guest_threads_quiescent(publish).map_err(std::io::Error::from_raw_os_error)?)
        })();
        if let Err(error)=result{return -(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64);}
        cursor=hi;
    }
    if cursor<addr+len{return discard_private_kernel(cursor,addr+len-cursor);}0
}
fn discard_private_kernel(addr: u64, len: u64) -> i64 {`],
  ['    copies::moved(old, new, moved);','    if keep_old{copies::aliased(old,new,moved);}else{copies::moved(old, new, moved);}'],
  ['                super::memfd::note_alias(old, at, new_len);','                super::memfd::note_alias(old, at, new_len);copies::aliased(old,at,new_len);'],
  ['fn extend_at(addr: u64, len: u64, like: u64, replace: bool) -> Result<(), i64> {',`fn extend_at(addr:u64,len:u64,like:u64,replace:bool)->Result<(),i64>{
    let owned=super::mutable_memory::mapping(like,1,0).map(|lease|lease.protection);
    extend_at_kernel(addr,len,like,replace)?;
    if let Some(protection)=owned{if let Err(error)=mutable_enroll(addr,len,protection){window::unmap(addr,len);super::mutable_memory::forget(addr,len);return Err(-(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64));}}
    Ok(())
}
fn extend_at_kernel(addr: u64, len: u64, like: u64, replace: bool) -> Result<(), i64> {`],
  ['const MADV_DONTNEED: u64 = 4;',`${enrollmentSource}\nconst MADV_DONTNEED: u64 = 4;`],
  ['    for r in vmmap::regions(src, src + len) {','    super::mutable_memory::relocated(src,dst,len).map_err(|error|-(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64))?;\n    for r in vmmap::regions(src, src + len) {'],

  ['    if keep_old{patch::reconcile_far(old,old_len);}else{patch::forget_far(old,old_len);}', '    if keep_old{patch::reconcile_far(old,old_len);}else{patch::forget_far(old,old_len);super::mutable_memory::forget(old,old_len);}'],
  ['        patch::forget_far(old+new_len,old_len-new_len);','        patch::forget_far(old+new_len,old_len-new_len);super::mutable_memory::forget(old+new_len,old_len-new_len);'],
 ]);
 prepareHostcallMemory(aim);
}

const enrollmentSource=String.raw`
// Called under the existing exclusive VM epoch. Metadata and staged bytes are
// completely prepared before the only public change: one fixed Mach remap.
fn mutable_protect(a:[u64;6])->Option<i64>{
    if !super::mutable_code::available(){return None;}
    let(start,length,protection)=(a[0],page_up(a[1]),host_prot(a[2]));
    if start%PAGE!=0||length==0||start.checked_add(length).is_none()||window::touches_reserved(start,start+length){return None;}
    let owned=super::mutable_memory::ranges(start,length);
    if !owned.is_empty(){
        let mut cursor=start;
        for(lo,hi,_)in owned{
            if cursor<lo{let result=mprotect_owned([cursor,lo-cursor,a[2],0,0,0]);if result<0{return Some(result);}}
            match super::mutable_memory::protect(lo,hi-lo,protection){Some(Ok(()))=>{},Some(Err(error))=>return Some(-(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64)),None=>return Some(-(errno::EFAULT as i64))}
            cursor=hi;
        }
        if cursor<start+length{return Some(mprotect_owned([cursor,start+length-cursor,a[2],0,0,0]));}
        return Some(0);
    }
    if protection&libc::PROT_WRITE==0||!(patch::has_original_words(start,length)||xrt::has_original_mapping(start,length)){return None;}
    let result=mutable_enroll(start,length,protection);
    Some(match result{Ok(())=>0,Err(error)=>-(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64)})
}
fn mutable_enroll(start:u64,length:u64,protection:i32)->std::io::Result<()>{
        let _transaction=super::mutable_memory::transaction();
        super::verity_pager::validate_protection(start,length,protection)?;
        for region in vmmap::regions(start,start+length){
            if !region.shared{continue;}
            let private=copies::find(region.start).is_some_and(|(copy,offset)|{
                copy.end>=region.end&&region.file.as_ref().is_some_and(|(_,dev,ino)|copy.dev==*dev&&copy.ino==*ino&&offset==region.offset)
            });
            if !private{
                // A genuine shared read-only source cannot acquire write by
                // replacing it with private memory. Shared writable code also
                // needs its original shared backing, never a silent COW copy.
                return Err(std::io::Error::from_raw_os_error(if protection&libc::PROT_WRITE!=0&&region.usage.max_prot&libc::PROT_WRITE as u32==0{libc::EACCES}else{libc::ENOEXEC}));
            }
        }
        let mut bytes=Vec::new();bytes.try_reserve_exact(length as usize).map_err(|_|std::io::Error::from_raw_os_error(libc::ENOMEM))?;bytes.resize(length as usize,0);
        let mut actual=0;let result=unsafe{mach_vm_read_overwrite(task(),start,length,bytes.as_mut_ptr()as u64,&mut actual)};
        if result!=0||actual!=length{return Err(std::io::Error::from_raw_os_error(libc::EFAULT));}
        patch::restore_original_copy(start,&mut bytes)?;xrt::restore_original_mapping(start,&mut bytes)?;
        // A successful remap becomes anonymous in Darwin's VM metadata.
        // Retain its exact clipped file identity/offset in the existing copy
        // owner so discard, move and fork still resolve the original source.
        let files:Vec<_>=vmmap::regions(start,start+length).filter_map(|region|{
            region.file.map(|(host,dev,ino)|{
                let guest=xrt::original_guest_path_of_host(&host).or_else(||crate::vfs::guest_path_of_host(&host)).unwrap_or_default();
                (region.start,region.end,region.offset,host,guest,dev,ino)
            })
        }).collect();
        let prepared=super::mutable_memory::prepare(start,&bytes,protection)?;
        prepared.install(|publish|super::thread::with_guest_threads_quiescent(publish).map_err(std::io::Error::from_raw_os_error)?)?;
        for(lo,hi,offset,host,guest,dev,ino)in files{copies::note_file(lo,hi-lo,&host,&guest,offset,(dev,ino));}
        // Historical far recipes remain process-lived for already fetched
        // entries. New fetches see NX original bytes through the precise owner.
        patch::forget_far(start,length);copies::forget_elf_sites(start,start+length);
        super::verity_pager::forget(start,length)?;Ok(())
}
`;
