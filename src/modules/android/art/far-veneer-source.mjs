import fs from 'node:fs';
import path from 'node:path';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';

/** Preserve original instruction semantics when no direct branch can reach a
 * free island. Publication and VM mutation retain the original owners. */
export function prepareFarVeneers(aim){
 const root=path.join(aim,'crates/aim-linux-abi/src');
 copyChanged(path.join(import.meta.dirname,'native/far-veneer.rs'),path.join(root,'far-veneer.rs'));
 copyChanged(path.join(import.meta.dirname,'native/executable-map.rs'),path.join(root,'sys/executable-map.rs'));
 const edit=(file,changes)=>{const target=path.join(root,file);let text=fs.readFileSync(target,'utf8');for(const[from,to,count=1]of changes){if(text.split(from).length!==count+1)throw Error('Original far veneer boundary changed: '+file+' '+from.slice(0,70));text=text.split(from).join(to);}writeChanged(target,text);};
 edit('a64.rs',[[
  'pub(crate) use x18::{terminal as x18_terminal,resume_stack as x18_resume_stack,evidence as instruction_evidence,elf_evidence as elf_instruction_evidence};',
  'pub(crate) use x18::{terminal as x18_terminal,resume_stack as x18_resume_stack,evidence as instruction_evidence,elf_evidence as elf_instruction_evidence,emit_far as x18_emit_far,far_capacity as x18_far_capacity,resume_stack_far as x18_resume_stack_far};'
 ]]);
 edit('patch.rs',[
  ['static ISLANDS: Mutex<Vec<Island>> = Mutex::new(Vec::new());',`static ISLANDS: Mutex<Vec<Island>> = Mutex::new(Vec::new());
mod far { use super::*; include!("far-veneer.rs"); }
pub struct PreparedRewrite {pub words:Vec<(u64,u32)>,pub stats:PatchStats,far:far::Prepared}
impl PreparedRewrite {
    pub fn publish(&mut self){self.far.publish();}
    pub fn commit(&mut self){self.far.commit();}
    fn apply(&mut self){self.publish();write_words(&self.words);self.commit();}
}
pub fn forget_far(start:u64,length:u64){far::forget(start,length);}
pub fn reconcile_far(start:u64,length:u64){far::reconcile(start,length);}
pub fn far_epoch()->u64{far::epoch()}
pub fn replace_far(start:u64,length:u64,epoch:u64){far::replace(start,length,epoch);}
pub fn relocate_far(old:u64,new:u64,length:u64)->io::Result<PreparedFarRelocation>{Ok(PreparedFarRelocation(far::relocation(old,new,length)?))}
pub struct PreparedFarRelocation(far::Prepared);
impl PreparedFarRelocation{pub fn publish(&mut self){self.0.publish();}pub fn commit(&mut self){self.0.commit();}}
pub fn check_original_instructions(base:u64,bytes:&[u8])->io::Result<()>{far::check_original(base,bytes)}
pub fn check_reprotected_instructions(base:u64,bytes:&[u8],existing:bool)->io::Result<()>{far::check_bytes(base,bytes,existing)}
pub fn far_history_sites(start:u64,length:u64)->Vec<u64>{far::history_sites(start,length)}
pub fn far_writable_words(start:u64,length:u64)->Vec<(u64,u32)>{far::writable_words(start,length)}
pub fn restore_far_for_write(start:u64,length:u64){let words=far::writable_words(start,length);write_words(&words);far::forget(start,length);}
pub unsafe fn resume_far_stack(context:*mut libc::c_void)->bool{unsafe{far::resume_stack(context)}}
pub unsafe fn hardware_brk(info:*const libc::siginfo_t,context:*mut libc::c_void)->bool{unsafe{
    if info.is_null()||context.is_null(){return false;}
    let uc=context as*mut libc::ucontext_t;if (*uc).uc_mcontext.is_null(){return false;}
    let machine=&*((*uc).uc_mcontext as*const MContext64);
    // Darwin delivers BRK with code zero. A software sender can retain stale
    // ESR, so require the kernel's fault sender and exact interrupted address.
    (*info).si_code>=0&&(*info).si_code<0x10000&&(*info).si_pid==0&&
        (*info).si_addr as u64==machine.ss.pc&&(machine.es[1]as u32)>>26==0x3c
}}`],
  ['    let(words,stats)=prepare_rewrite_sites(sites,lo,hi,use_islands)?;\n    write_words(&words);Ok(stats)',
   '    let mut prepared=prepare_rewrite_sites(sites,lo,hi,use_islands)?;prepared.apply();Ok(prepared.stats)'],
  ['io::Result<(Vec<(u64,u32)>,PatchStats)>','io::Result<PreparedRewrite>',4],
  ['    if crate::a64_execution::enabled(){return Ok((Vec::new(),PatchStats::default()));}',
   '    if crate::a64_execution::enabled(){return Ok(PreparedRewrite{words:Vec::new(),stats:PatchStats::default(),far:far::Prepared::empty()});}',2],
  ['    let mut site_words = Vec::with_capacity(sites.len());','    let mut site_words = Vec::with_capacity(sites.len());\n    let mut pending_far=far::Prepared::empty();'],
  ['        let word = placed.and_then(|i| {','        let mut word = placed.and_then(|i| {'],
  ['        if word.is_none() && kind==Kind::X18 && a64::x18_terminal(rt).is_none() {',`        if use_islands && (stage=="elf-sites"||stage=="elf-map") && placed.is_none() && kind==Kind::X18 && a64::x18_terminal(rt).is_none() {
            word=Some(pending_far.add(&mut islands,site,rt,ctr)?);stats.brk_fallback+=1;
        }
        if word.is_none() && kind==Kind::X18 && a64::x18_terminal(rt).is_none() {`],
  ['    Ok((site_words,stats))','    Ok(PreparedRewrite{words:site_words,stats,far:pending_far})'],
  ['    if (kind!=Kind::X18 || a64::x18_terminal(rt).is_some()) && word==a64::brk_fallback(kind,rt){return true;}',
   '    if kind==Kind::X18&&far::valid(site,rt,word){return true;}\n    if (kind!=Kind::X18 || a64::x18_terminal(rt).is_some()) && word==a64::brk_fallback(kind,rt){return true;}'],
  ['    let (words,stats)=prepare_region(addr,bytes,use_islands)?;\n    write_words(&words);Ok(stats)',
   '    let mut prepared=prepare_region(addr,bytes,use_islands)?;prepared.apply();Ok(prepared.stats)'],
  ['pub unsafe fn handle_brk(uc: *mut libc::ucontext_t) -> bool {',
   'pub unsafe fn handle_brk(uc: *mut libc::ucontext_t) -> bool {\n    if unsafe{far::handle(uc)}{return true;}'],
  ['pub(crate) fn exec_reset() {','pub(crate) fn exec_reset() {\n    far::reset();'],
  ['    w.seq(islands.iter(), |w, isl| {\n        w.u64(isl.rx);\n        w.u64(isl.used);\n        w.u64(isl.size);\n    });',
   '    w.seq(islands.iter(), |w, isl| {\n        w.u64(isl.rx);\n        w.u64(isl.used);\n        w.u64(isl.size);\n    });\n    far::save(w);'],
  ['    }\n}\n\ninclude!("island-placement.rs");','    }\n    far::restore(r,&islands);\n}\n\ninclude!("island-placement.rs");'],
 ]);
 edit('sys/signal.rs',[
  ['(*info).si_code < 0x10000 && crate::a64::x18_resume_stack(uc) { return; }',
   '(*info).si_code < 0x10000 && (crate::a64::x18_resume_stack(uc)||crate::patch::resume_far_stack(uc)) { return; }'],
  ['if sig == libc::SIGTRAP && crate::patch::handle_brk(uc as *mut libc::ucontext_t) {',
   'if sig == libc::SIGTRAP && crate::patch::hardware_brk(info,uc) && crate::patch::handle_brk(uc as *mut libc::ucontext_t) {'],
  ['    ) && si.si_code > 0\n        && si.si_code < 0x10000;',
   '    ) && ((si.si_code > 0 && si.si_code < 0x10000) || (hsig==libc::SIGTRAP&&unsafe{crate::patch::hardware_brk(si,uc)}));'],
 ]);
 edit('sys/jit.rs',[
  ['        let prepared=patch::prepare_region_at(lo,bytes,true,"jit-final");',
   '        let prepared=patch::check_original_instructions(lo,bytes).and_then(|_|patch::prepare_region_at(lo,bytes,true,"jit-final"));'],
  ['    if keep && prot & libc::PROT_EXEC != 0 {\n        let bytes=', '    let mut translation=None;\n    if keep && prot & libc::PROT_EXEC != 0 {\n        let bytes='],
  ['        let (words,stats)=match prepared {','        let prepared=match prepared {'],
  ['        super::mem::trace_stats("prepared jit-final",lo,n,&stats);\n        for (site,word) in words {\n            unsafe{tmp.cast::<u8>().add((site-lo)as usize).cast::<u32>().write(word);}\n        }',
   '        super::mem::trace_stats("prepared jit-final",lo,n,&prepared.stats);\n        for &(site,word) in &prepared.words {\n            unsafe{tmp.cast::<u8>().add((site-lo)as usize).cast::<u32>().write(word);}\n        }\n        translation=Some(prepared);'],
  ['    let (mut at, mut cur, mut max) = (lo, 0, 0);','    if let Some(prepared)=translation.as_mut(){prepared.publish();}\n    let (mut at, mut cur, mut max) = (lo, 0, 0);'],
  ['    if keep && prot & libc::PROT_EXEC != 0 {\n        unsafe { sys_icache_invalidate',
   '    patch::forget_far(lo,n);if let Some(prepared)=translation.as_mut(){prepared.commit();}\n    if keep && prot & libc::PROT_EXEC != 0 {\n        unsafe { sys_icache_invalidate'],
 ]);
 edit('sys/verity_pager.rs',[
  ['        let (prepared, stats) = crate::patch::prepare_rewrite_sites(&targets,lo,hi,true)?;\n        let prepared: std::collections::BTreeMap<_, _> = prepared.into_iter().collect();',
   '        let mut publication=crate::patch::prepare_rewrite_sites(&targets,lo,hi,true)?;publication.publish();\n        let stats=publication.stats;let prepared:std::collections::BTreeMap<_,_>=publication.words.iter().copied().collect();'],
  ['Ok((Arc::new(Self::restore(plan, bias, words)?), stats))',
   '        let pages=Self::restore(plan,bias,words)?;publication.commit();Ok((Arc::new(pages),stats))'],
 ]);
 edit('sys/mem.rs',[
  ['fn mmap_guest(mut a: [u64; 6]) -> i64 {','fn mmap_guest(mut a: [u64; 6]) -> i64 {\n    let far_epoch=patch::far_epoch();'],
  ['        let result = mmap_owned(a);','        let result=mmap_owned(a);\n        if result>=0{patch::replace_far(result as u64,page_up(a[1]),far_epoch);}'],
  ['let result = mmap_pinned(a, &held);','    let result=mmap_pinned(a,&held);\n    if result>=0{patch::replace_far(result as u64,page_up(a[1]),far_epoch);}'],
  ['fn mmap_kernel(a: [u64; 6]) -> i64 {','include!("executable-map.rs");\n\nfn mmap_kernel(a: [u64; 6]) -> i64 {'],
  ['    let len = page_up(len);\n    let mut addr = addr;',
   '    let len = page_up(len);\n    if flags&MAP_FIXED!=0&&flags&MAP_ANONYMOUS==0&&prot&PROT_EXEC!=0{if let Err(error)=validate_fixed_executable_file(addr,len,fd,off){return error;}}\n    let mut addr = addr;'],
  ['        if let Some(ExecSource::Shared) = source {',
   '        if fixed && !anon && exec && !writable && !matches!(source,Some(ExecSource::Shared)) {\n            return preflight_executable_map(addr,len,host_prot(prot),fd,off,source.as_ref().unwrap());\n        }\n        if let Some(ExecSource::Shared) = source {'],
  ['                if exec {\n                    crate::diag::register_fd_module',
   '                if exec {\n                    let bytes=unsafe{std::slice::from_raw_parts(b as*const u8,len as usize)};\n                    if let Err(error)=patch::check_original_instructions(b,bytes){window::unmap(b,len);return -(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64);}\n                    crate::diag::register_fd_module'],
  ['    if result == 0 {\n        vm_account::with(|s| s.unmap(addr, addr + len));\n        super::vma_names::forget(addr, addr + len);\n        super::fuse_cache::forget(addr, len);',
   '    if result == 0 {\n        vm_account::with(|s| s.unmap(addr, addr + len));\n        super::vma_names::forget(addr, addr + len);\n        patch::forget_far(addr,len);\n        super::fuse_cache::forget(addr,len);'],
  ['            crate::diag::register_fd_module(b, len, fd, off);\n            (b, Finish::Protect)',
   '            if !fixed{let bytes=unsafe{std::slice::from_raw_parts(b as*const u8,len as usize)};if let Err(error)=patch::check_original_instructions(b,bytes){window::unmap(b,len);return -(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64);}}\n            crate::diag::register_fd_module(b, len, fd, off);\n            (b, Finish::Protect)'],
  ['    let rwx = prot & (PROT_WRITE | PROT_EXEC) == PROT_WRITE | PROT_EXEC;\n    if prot & PROT_EXEC != 0 {',
   '    let rwx = prot & (PROT_WRITE | PROT_EXEC) == PROT_WRITE | PROT_EXEC;\n    if rwx&&!patch::far_writable_words(addr,len).is_empty(){crate::diag!("[linux-abi] executable publication rejected: immutable far instruction region requested writable execution");return -(errno::ENOEXEC as i64);}\n    if prot & PROT_EXEC != 0 {'],
  ['                let known=copies::file_copies(lo,hi);',
   '                let bytes=unsafe{std::slice::from_raw_parts(lo as*const u8,(hi-lo)as usize)};\n                if let Err(error)=patch::check_reprotected_instructions(lo,bytes,cur_prot&libc::PROT_WRITE==0){host_mprotect(lo,hi-lo,cur_prot);return -(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64);}\n                let known=copies::file_copies(lo,hi);'],
  ['    let ranges = super::verity_pager::covered(addr, len);\n    if !ranges.is_empty() {',
   '    if prot&(PROT_WRITE|PROT_EXEC)==(PROT_WRITE|PROT_EXEC)&&!patch::far_writable_words(addr,len).is_empty(){crate::diag!("[linux-abi] executable publication rejected: immutable far instruction region requested writable execution");return -(errno::ENOEXEC as i64);}\n    let ranges = super::verity_pager::covered(addr, len);\n    if !ranges.is_empty() {'],
  ['        return 0;\n    }\n    let rwx = prot & (PROT_WRITE | PROT_EXEC)',
   '        if prot&PROT_WRITE!=0&&prot&PROT_EXEC==0{patch::restore_far_for_write(addr,len);copies::forget_elf_sites(addr,addr+len);}\n        return 0;\n    }\n    let rwx = prot & (PROT_WRITE | PROT_EXEC)'],
  ['if r == 0 {\n        super::verity_pager::note_protection(addr, len, host_prot(prot));\n    }',
   '    if r==0{if prot&PROT_WRITE!=0&&prot&PROT_EXEC==0{patch::restore_far_for_write(addr,len);copies::forget_elf_sites(addr,addr+len);}\n        super::verity_pager::note_protection(addr,len,host_prot(prot));}'],
  ['    let moved = old_len.min(new_len);\n    remap_shared(new, old, moved)?;',
   '    let moved = old_len.min(new_len);\n    let mut far=patch::relocate_far(old,new,moved).map_err(|error|-(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64))?;far.publish();\n    remap_shared(new, old, moved)?;\n    patch::forget_far(new,moved);far.commit();'],
  ['        window::unmap(old, old_len);\n    }\n    Ok(())',
   '        window::unmap(old, old_len);\n    }\n    if keep_old{patch::reconcile_far(old,old_len);}else{patch::forget_far(old,old_len);}\n    Ok(())'],
  ['let result = super::verity_pager::remap(old, old_len, destination, new_len, keep, || {',
   '        let mut far=match patch::relocate_far(old,destination,old_len.min(new_len)){Ok(prepared)=>prepared,Err(error)=>return -(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64)};far.publish();\n        let result=super::verity_pager::remap(old,old_len,destination,new_len,keep,|| {'],
  ['        return match result {\n            Ok(true) => destination as i64,\n            Ok(false) => -(libc::EFAULT as i64),\n            Err(error) => -(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO)) as i64),\n        };',
   '        return match result {Ok(true)=>{patch::forget_far(destination,new_len);if destination!=old{if keep{patch::reconcile_far(old,old_len);}else{patch::forget_far(old,old_len);}}else if new_len<old_len{patch::forget_far(old+new_len,old_len-new_len);}far.commit();destination as i64},Ok(false)=>-(libc::EFAULT as i64),Err(error)=>-(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64)};'],
  ['        return match remap_shared(at, old, new_len) {',
   '        let mut far=match patch::relocate_far(old,at,new_len){Ok(prepared)=>prepared,Err(error)=>return -(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64)};far.publish();\n        return match remap_shared(at, old, new_len) {'],
  ['                super::memfd::note_alias(old, at, new_len);\n                at as i64',
   '                super::memfd::note_alias(old, at, new_len);\n                patch::forget_far(at,new_len);far.commit();at as i64'],
  ['            copies::forget(old + new_len, old + old_len);','            copies::forget(old + new_len, old + old_len);\n            patch::forget_far(old+new_len,old_len-new_len);'],
 ]);
}
