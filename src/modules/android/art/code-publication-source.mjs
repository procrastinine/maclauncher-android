import fs from 'node:fs';
import path from 'node:path';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';

/** Translate final JIT bytes while the original VM owner still has a private
 * staging copy. Publish the rewritten copy in the existing atomic remap. */
export function prepareCodePublication(aim){
 const root=path.join(aim,'crates/aim-linux-abi/src');
 copyChanged(path.join(import.meta.dirname,'native/publication-diagnostics.rs'),path.join(root,'publication-diagnostics.rs'));
 const edit=(file,changes)=>{const target=path.join(root,file);let source=fs.readFileSync(target,'utf8');
  for(const[from,to]of changes){if(source.split(from).length!==2)throw Error('AIM executable publication boundary changed: '+file+' '+from.slice(0,60));source=source.replace(from,to);}
  writeChanged(target,source);
 };
 edit('patch.rs',[
  [`    let start = (addr + 3) & !3;
    let mut sites = Vec::new();
    let words=((addr+len-start)/4)as usize;
    let bytes=unsafe {std::slice::from_raw_parts(start as *const u8,words*4)};`,
   `    let bytes=unsafe {std::slice::from_raw_parts(addr as *const u8,len as usize)};
    let (words,stats)=prepare_region(addr,bytes,use_islands)?;
    write_words(&words);Ok(stats)
}

/// Analyze staging bytes using their final guest address. Veneer branches and
/// PC-relative operands belong to the destination, never the temporary copy.
pub(crate) fn prepare_region(addr:u64,bytes:&[u8],use_islands:bool)->io::Result<(Vec<(u64,u32)>,PatchStats)> {
    prepare_region_at(addr,bytes,use_islands,"runtime-region")
}
pub(crate) fn prepare_region_at(addr:u64,bytes:&[u8],use_islands:bool,stage:&str)->io::Result<(Vec<(u64,u32)>,PatchStats)> {
    let len=bytes.len()as u64;
    let start=(addr+3)&!3;
    let bytes=&bytes[(start-addr)as usize..];
    let words=bytes.len()/4;
    let mut sites=Vec::new();`],
  ['        let insn = unsafe { (site as *const u32).read() };',
   '        let offset=(site-start)as usize;\n        let insn=u32::from_le_bytes(bytes[offset..offset+4].try_into().unwrap());'],
  ['    rewrite_sites(&sites, addr, addr + len, use_islands)','    prepare_rewrite_sites_at(&sites,addr,addr+len,use_islands,stage)'],
  ['pub fn prepare_rewrite_sites(\n', 'include!("publication-diagnostics.rs");\n\npub fn prepare_rewrite_sites(\n'],
  [`    use_islands: bool,
) -> io::Result<(Vec<(u64,u32)>,PatchStats)> {
    let mut stats`, `    use_islands: bool,
) -> io::Result<(Vec<(u64,u32)>,PatchStats)> {
    prepare_rewrite_sites_at(sites,lo,hi,use_islands,"elf-sites")
}
fn prepare_rewrite_sites_at(sites:&[(u64,Kind,u32)],lo:u64,hi:u64,use_islands:bool,stage:&str)->io::Result<(Vec<(u64,u32)>,PatchStats)> {
    let mut stats`],
  ['            return Err(io::Error::from_raw_os_error(if placed.is_none(){libc::ENOMEM}else{libc::ENOEXEC}));',
   '            let errno=if placed.is_none(){libc::ENOMEM}else{libc::ENOEXEC};\n            trace_publication_rejection(stage,site,lo,hi,rt,placed.is_some(),errno);\n            return Err(io::Error::from_raw_os_error(errno));'],
 ]);
 edit('sys/jit.rs',[
  ['        libc::mprotect(tmp, n as usize, prot);\n    }\n    let (mut at, mut cur, mut max) = (lo, 0, 0);',
   `    }
    if keep && prot & libc::PROT_EXEC != 0 {
        let bytes=unsafe{std::slice::from_raw_parts(tmp.cast::<u8>(),n as usize)};
        let prepared=patch::prepare_region_at(lo,bytes,true,"jit-final");
        let (words,stats)=match prepared {
            Ok(prepared)=>prepared,
            Err(error)=>{unsafe{libc::munmap(tmp,n as usize);}
                return -(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64);}
        };
        super::mem::trace_stats("prepared jit-final",lo,n,&stats);
        for (site,word) in words {
            unsafe{tmp.cast::<u8>().add((site-lo)as usize).cast::<u32>().write(word);}
        }
    }
    if unsafe{libc::mprotect(tmp,n as usize,prot)}!=0 {
        let result=errno::check(-1);unsafe{libc::munmap(tmp,n as usize);}return result;
    }
    let (mut at, mut cur, mut max) = (lo, 0, 0);`],
 ]);
 edit('sys/mem.rs',[
  ['fn trace_stats(what: &str, addr: u64, len: u64, stats: &patch::PatchStats) {',
   'pub(super) fn trace_stats(what: &str, addr: u64, len: u64, stats: &patch::PatchStats) {'],
  ['if crate::sys::tracing() && stats.total() > 0 {',
   'if crate::sys::tracing() && (stats.total() > 0 || what == "prepared jit-final") {'],
  ['{} scs, {} ctr_el0, {} brk fallbacks",',
   '{} scs, {} ctr_el0, {} x18, {} brk fallbacks",'],
  ['            stats.ctr,\n            stats.brk_fallback',
   '            stats.ctr,\n            stats.x18,\n            stats.brk_fallback'],
 ]);
}
