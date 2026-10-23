import fs from 'node:fs';
import path from 'node:path';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';
import {prepareInitialFiles} from './initial-file-source.mjs';

/** Add sparse original-byte publication after the existing mutable owner. */
export function prepareInitialExecution(aim){
 const root=path.join(aim,'crates/aim-linux-abi/src');
 const pending=new Map();
 const edit=(name,changes)=>{const file=path.join(root,name);let text=pending.get(file)??fs.readFileSync(file,'utf8');for(const[from,to,count=1]of changes){if(text.split(from).length!==count+1)throw Error('Initial instruction owner boundary changed: '+name+' '+from.slice(0,100));text=text.split(from).join(to);}pending.set(file,text);};
 edit('sys/mod.rs',[['pub(crate) mod mutable_memory;','pub(crate) mod mutable_memory;\n#[path="initial-execution.rs"]\npub(crate) mod initial_execution;']]);
 edit('sys/mutable-memory.rs',[["#[cfg(test)]\nmod tests {",'include!("initial-memory.rs");\n\n#[cfg(test)]\nmod tests {']]);
 edit('sys/mutable-code.rs',[
  ['struct Memory {leases:Vec<memory::Lease>,pending:Option<(u64,i32)>,wrote:bool}',
   'struct Memory {leases:Vec<memory::Lease>,pending:Option<(u64,i32)>,wrote:bool,private_data:super::super::initial_execution::PrivateDataCache}'],
  ['    unsafe extern "C" fn counter(',`    unsafe extern "C" fn revalidate(opaque:*mut c_void,address:u64,length:usize,result:*mut Mapping)->i32{
        // Fresh owner authority is required for every inert table. Missing
        // ranges never invoke speculative pager admission. A private data
        // view must still pass this interval's exact physical classification.
        if memory::mapping(address,length as u64,R|X).is_some(){return unsafe{map(opaque,address,length,(R|X)as u32,result)};}
        let owner=unsafe{&mut*opaque.cast::<Memory>()};
        if owner.private_data.eligible(address,length as u64){return unsafe{map(opaque,address,length,(R|W)as u32,result)};}
        -errno::EFAULT
    }
    unsafe extern "C" fn counter(`],
  ['assert!(!handle.is_null(),"original A64 precise component");Cpu{handle,memory}',
   'assert!(!handle.is_null(),"original A64 precise component");assert_eq!(unsafe{kl_a64_precise_set_owned_cache(handle,Some(revalidate))},0);Cpu{handle,memory}'],
  ['    fn clear_exclusive(){',
   '    fn reset_native(){with_cpu(|cpu|assert_eq!(unsafe{kl_a64_precise_reset(cpu.handle)},0,"original A64 native state reset"));}\n    fn clear_exclusive(){'],
  ['        clear_exclusive();unsafe{state.to_context(ctx,raw);}drop(frame);context::resume_native_trap()',
   '        reset_native();unsafe{state.to_context(ctx,raw);}drop(frame);context::resume_native_trap()'],
  ['let frame=Frame::enter();clear_exclusive();',
   'let frame=Frame::enter();reset_native();'],
  ['cpu.memory.wrote=false;cpu.memory.pending=None;cpu.memory.leases.clear();',
   'cpu.memory.wrote=false;cpu.memory.pending=None;cpu.memory.leases.clear();cpu.memory.private_data.clear();'],
  ['cpu.memory.leases.clear();(result,instruction,wrote,pending)',
   'cpu.memory.leases.clear();cpu.memory.private_data.clear();(result,instruction,wrote,pending)'],
  // Preserve the shared overlay's flags:0 anchor; it adds its IO bit later.
  ['permissions:lease.protection as u32,flags:0}',
   'permissions:lease.protection as u32,flags:0|if lease.protection&X!=0{8}else{0}}'],
  ['flags:if protection&7==7&&sys::arena::contains(page,end-page){1}else{2}',
   'flags:(if protection&7==7&&sys::arena::contains(page,end-page){1}else{2})|if owner.private_data.eligible(page,end-page){4}else{0}'],
  ['                    let result=unsafe{kl_a64_precise_run(cpu.handle,&mut state,STEP.fetch_add(1,Ordering::Relaxed))};',`                    let before=state.pc;
                    let result=unsafe{if !raw&&let Some(lease)=memory::mapping(state.pc,1,X){kl_a64_precise_run_owned(cpu.handle,&mut state,STEP.fetch_add(1,Ordering::Relaxed),lease.start,lease.start+lease.length,super::super::initial_execution::budget())}else{kl_a64_precise_run(cpu.handle,&mut state,STEP.fetch_add(1,Ordering::Relaxed))}};
                    super::super::initial_execution::progress(before,state.pc,unsafe{kl_a64_precise_instruction(cpu.handle)},state.x[..4].try_into().unwrap());`],
 [
  '            match result.kind {',
  `            if result.kind>=2||result.kind==1&&result.detail!=0{super::super::initial_execution::report_stop(result.kind,result.detail,result.address,result.error,state.pc,instruction);}
            match result.kind {`
 ]]);
 edit('sys/executable-map.rs',[[
  '    let prepared=match source {',
  `    if super::initial_execution::enabled(){
        let sites=match source {
            ExecSource::LoadTime(Some(sites))=>sites.sites.iter().filter(|&&(at,_,_)|at>=offset&&at.checked_add(4).is_some_and(|end|end<=offset+length)).map(|&(at,_,_)|address+at-offset).collect(),
            ExecSource::LoadTime(None)=>match super::initial_execution::scan_sites(address,bytes){Ok(sites)=>sites,Err(error)=>return -(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64)},
            ExecSource::Shared=>unreachable!(),
        };
        if let Err(error)=super::initial_execution::uniform(address,bytes,protection,&sites){return -(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64);}
        crate::diag::register_fd_module(address,length,fd,offset);
        copies::forget(address,address+length);super::fuse_cache::forget(address,length);note_file_copy(address,length,fd,offset);return address as i64;
    }
    let prepared=match source {`
 ]]);
 edit('sys/mem.rs',[
  ['Some(Ok(()))=>{},Some(Err(error))=>return Some(', 'Some(Ok(()))=>{super::verity_pager::note_initial_protection(lo,hi-lo,protection);},Some(Err(error))=>return Some('],
  ['pub(super) fn reprotect_verified_page(address:u64,length:u64,protection:i32)->std::io::Result<()> {',`pub(super) fn reprotect_verified_page(address:u64,length:u64,protection:i32)->std::io::Result<()> {
    if super::initial_execution::enabled()&&super::mutable_memory::covers(address,length){return super::mutable_memory::protect(address,length,protection).unwrap();}`],
  ['fn rewrite_file_copy(b: u64, len: u64, fd: i32, off: u64, source: &ExecSource) -> Result<(),i64> {',`fn rewrite_file_copy(b: u64, len: u64, fd: i32, off: u64, source: &ExecSource, protection:i32) -> Result<(),i64> {
    if super::initial_execution::enabled(){return super::initial_execution::file_copy(b,len,fd,off,source,protection).map_err(|error|-(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64));}`],
  ['rewrite_file_copy(b,len,fd,off,&source)','rewrite_file_copy(b,len,fd,off,&source,host_prot(prot))'],
  ['rewrite_file_copy(begin,end-begin,fd,offset,&xrt::exec_source(fd,offset))?;','rewrite_file_copy(begin,end-begin,fd,offset,&xrt::exec_source(fd,offset),host_prot(prot))?;'],
  ['                host_mprotect(base, len, host_prot(prot))','                if super::initial_execution::enabled(){match super::initial_execution::protect(base,len,host_prot(prot)){Ok(())=>0,Err(error)=>-(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64)}}else{host_mprotect(base, len, host_prot(prot))}'],
  ['                let known=copies::file_copies(lo,hi);',`                if super::initial_execution::enabled(){
                    if let Err(error)=super::initial_execution::region_copy(lo,hi-lo,host_prot(prot)){host_mprotect(lo,hi-lo,cur_prot);return -(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64);}
                    cur=hi;continue;
                }
                let known=copies::file_copies(lo,hi);`],
  ['    let r = host_mprotect(addr, len, host_prot(prot));','    let r=if super::initial_execution::enabled(){match super::initial_execution::protect(addr,len,host_prot(prot)){Ok(())=>0,Err(error)=>-(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64)}}else{host_mprotect(addr,len,host_prot(prot))};'],
 ]);
 edit('loader.rs',[
  ['        let plan=xrt::prepare_verified_exec(original.clone(),Path::new(path),0,length).map_err(|error|format!("{name}: Linux errno {error}"))?;',`        let plan=if crate::sys::initial_execution::enabled(){xrt::verified::prepare_verified_rewrite(original.clone(),Path::new(path),&(0..length)).map(xrt::VerifiedExecPlan::Rewrite)}else{xrt::prepare_verified_exec(original.clone(),Path::new(path),0,length)}.map_err(|error|format!("{name}: Linux errno {error}"))?;`],
  ['        let prot = prot_of(p.p_flags);\n        // SAFETY: final protection of the segment we mapped.',`        let prot = prot_of(p.p_flags);
        if crate::sys::initial_execution::enabled()&&prot&libc::PROT_EXEC!=0&&p.p_filesz>0{
            let length=page_up(file_end)-start;let offset=page_down(p.p_offset);let source=xrt::exec_source(fd,offset);
            crate::sys::initial_execution::file_copy(start,length,fd,offset,&source,prot).map_err(|error|format!("{name}: original instruction view: {error}"))?;
        }
        // SAFETY: final protection of the segment we mapped.`],
  ['        if unsafe { libc::mprotect(start as *mut _, (mem_end - start) as usize, prot) } != 0 {','        if if crate::sys::initial_execution::enabled(){crate::sys::initial_execution::protect(start,mem_end-start,prot).is_err()}else{(unsafe{libc::mprotect(start as*mut _,(mem_end-start)as usize,prot)})!=0} {'],
  ['    let stats = match analysis {',`    if crate::sys::initial_execution::enabled(){
        let mut sites=Vec::new();
        if let Some(analysis)=&analysis{sites.extend(analysis.sites.iter().map(|site|bias+site.vaddr));}else{
            for segment in h.loads.iter().filter(|segment|segment.p_flags&PF_X!=0){let bytes=unsafe{std::slice::from_raw_parts((bias+segment.p_vaddr)as*const u8,segment.p_filesz as usize)};sites.extend(crate::sys::initial_execution::scan_sites(bias+segment.p_vaddr,bytes).map_err(|error|format!("{name}: original instruction analysis: {error}"))?);}
        }
        let protections:Vec<_>=(bias+h.lo..bias+h.hi).step_by(PAGE as usize).map(|page|page_prot.get(&page).copied().unwrap_or(libc::PROT_NONE)).collect();
        let bytes=unsafe{std::slice::from_raw_parts((bias+h.lo)as*const u8,span as usize)}.to_vec();
        crate::sys::initial_execution::publish(bias+h.lo,&bytes,&protections,&sites).map_err(|error|format!("{name}: original instruction publication: {error}"))?;
        return Ok(patch::PatchStats::default());
    }
    let stats = match analysis {`],
 ]);
 // The authenticated pager keeps its proof and site records. Initial mode
 // publishes originals instead of applying a relocation-dependent derivative.
 edit('sys/verity_pager.rs',[
  ['pub fn note_protection(start:u64,length:u64,protection:i32){',`pub fn note_initial_protection(start:u64,length:u64,protection:i32){
    let mappings=MAPPINGS.lock().unwrap();for registration in mappings.iter(){let map=unsafe{&*registration.mapping};let lo=start.max(map.base);let hi=start.saturating_add(length).min(map.base+map.length);if lo>=hi{continue;}let mut pages=map.operation.lock().unwrap();for at in(lo..hi).step_by(PAGE as usize){pages[((at-map.base)/PAGE)as usize].protection=protection;}}
}
pub fn note_protection(start:u64,length:u64,protection:i32){`],
  ['    generation:u64, base: u64, length: u64, offset: u64, logical_protection: i32, shared: bool,','    generation:u64, base: u64, length: u64, offset: u64, logical_protection: i32, shared: bool,\n    initial_pages:std::sync::OnceLock<Result<Option<Arc<RewritePages>>,i32>>,'],
  ['shared:record.old.shared,source,retired:', 'shared:record.old.shared,source,initial_pages:std::sync::OnceLock::new(),retired:'],
  ['logical_protection: protection, shared, source, retired:', 'logical_protection: protection, shared, source, initial_pages:std::sync::OnceLock::new(), retired:'],
  ['        let sites=Self::sites(&plan,bias)?;let targets:Vec<_>=sites.iter().map(|&(_,address,kind,rt)|(address,kind,rt)).collect();',`        let sites=Self::sites(&plan,bias)?;
        if super::initial_execution::enabled(){let words=sites.iter().map(|&(offset,_,_,_)|{let at=offset as usize;(offset,u32::from_le_bytes(plan.original_bytes()[at..at+4].try_into().unwrap()))}).collect();return Ok((Arc::new(Self::restore(plan,bias,words)?),crate::patch::PatchStats::default()));}
        let targets:Vec<_>=sites.iter().map(|&(_,address,kind,rt)|(address,kind,rt)).collect();`],
  ['            if offset!=word_offset||!crate::patch::validate_prepared_word(address,kind,rt,word){return Err(io::Error::from_raw_os_error(libc::EIO));}',`            let original=plan.original_bytes().get(offset as usize..offset as usize+4).and_then(|bytes|bytes.try_into().ok()).map(u32::from_le_bytes);
            if offset!=word_offset||if super::initial_execution::enabled(){original!=Some(word)}else{!crate::patch::validate_prepared_word(address,kind,rt,word)}{return Err(io::Error::from_raw_os_error(libc::EIO));}`],
  ['        let hash=plan.sites.fips.as_ref().map(|module|{let hash=module.digest_of(&patched);module.reinject(&mut patched);(plan.binding.source_offset+module.hash_offset,hash)});','        let hash=if super::initial_execution::enabled(){None}else{plan.sites.fips.as_ref().map(|module|{let hash=module.digest_of(&patched);module.reinject(&mut patched);(plan.binding.source_offset+module.hash_offset,hash)})};'],
  ['    let cache=match map.source.cache.page(&map.source,offset,&verified){',`    if super::initial_execution::enabled()&&!map.shared&&protection&libc::PROT_EXEC!=0{
        let original_sites=|pages:&RewritePages|pages.words.iter().filter_map(|&(at,_)|{let at=at+pages.plan.binding.source_offset;(at>=offset&&at.checked_add(4).is_some_and(|end|end<=offset+PAGE)).then_some(page+at-offset)}).collect::<Vec<_>>();
        let sites:Vec<_>=match &map.source.derivative{
            Some(Derivative::Rewrite(pages))=>original_sites(pages),
            Some(Derivative::Composed(pages))=>match RewritePages::sites(&pages.rewrite.plan,pages.rewrite.bias){Ok(sites)=>sites.into_iter().filter_map(|(_,at,_,_)|(at>=page&&at+4<=page+PAGE).then_some(at)).collect(),Err(_)=>return FaultResult::Bus{address}},
            Some(Derivative::Artifact(_))=>return FaultResult::Bus{address},
            None=>match map.initial_pages.get_or_init(||super::initial_execution::verified_pages(&map.source,offset).map_err(|error|error.raw_os_error().unwrap_or(libc::EIO))){
                Ok(Some(pages))=>original_sites(pages),Ok(None)=>match super::initial_execution::scan_sites(page,&verified){Ok(sites)=>sites,Err(_)=>return FaultResult::Bus{address}},Err(_)=>return FaultResult::Bus{address},
            },
        };
        if !sites.is_empty(){
            use std::os::fd::AsRawFd;
            let descriptor=map.source.data.as_raw_fd();let Some(host)=crate::xrt::fd_path(descriptor)else{return FaultResult::Bus{address};};
            let identity=match crate::cache::FileStat::of_fd(descriptor){Ok((stat,_))=>stat,Err(_)=>return FaultResult::Bus{address}};
            let guest=crate::xrt::original_guest_path_of_host(&host).or_else(||crate::vfs::guest_path_of_host(&host)).unwrap_or_default();
            let mut bytes=vec![0;PAGE as usize];bytes[..verified.len()].copy_from_slice(&verified);
            let result=super::initial_execution::uniform(page,&bytes,protection,&sites);return match result{Ok(())=>{super::copies::note_file(page,PAGE,&host,&guest,offset,(identity.dev,identity.ino));ready[index].ready=true;FaultResult::Verified},Err(_)=>FaultResult::Bus{address}};
        }
    }
    let cache=match map.source.cache.page(&map.source,offset,&verified){`],
 ]);
 for(const name of ['initial-memory','initial-execution'])copyChanged(path.join(import.meta.dirname,'native',name+'.rs'),path.join(root,'sys',name+'.rs'));
 for(const[file,text]of pending)writeChanged(file,text);
 prepareInitialFiles(aim);
}
