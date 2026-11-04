import fs from 'node:fs';
import path from 'node:path';
import {prepareX18Core} from '../native/x18-source.mjs';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';
import {prepareAimThreadPointerEmitter} from '../native/thread-pointer-source.mjs';

/** The common instruction compiler is linked into the original AIM AOT and
 * load-time paths. AIM remains the sole ELF, slot, context and mapping owner. */
export function prepareAimRegisters({aim,klepton}) {
 const crate=path.join(aim,'crates/aim-linux-abi'),native=path.join(import.meta.dirname,'native');
 prepareX18Core(klepton,path.join(crate,'src/x18-core'));
 copyChanged(path.join(native,'x18.rs'),path.join(crate,'src/x18.rs'));
 const build=path.join(crate,'build.rs'),original=fs.readFileSync(build,'utf8');
 writeChanged(build,original.replace('fn main() {','fn main() {\n    build_x18();')+'\n'+fs.readFileSync(path.join(native,'x18-build.rs'),'utf8'));
 const edit=(file,changes)=>{
  const target=path.join(crate,file);let source=fs.readFileSync(target,'utf8');
  for(const[from,to]of changes){if(source.split(from).length!==2)throw Error('AIM register translation boundary changed: '+file+' '+from.slice(0,60));source=source.replace(from,to);}
  writeChanged(target,source);
 };
 edit('src/a64.rs',[
  ['pub enum Kind {','#[path = "x18.rs"]\nmod x18;\n\npub enum Kind {'],
  ['    MrsCtr = 5,','    MrsCtr = 5,\n    X18 = 6,'],
  ['pub const ALL: [Kind; 6]','pub const ALL: [Kind; 7]'],
  ['        Kind::MrsCtr,','        Kind::MrsCtr,\n        Kind::X18,'],
  ['            Kind::MrsCtr => "mrs_ctr_el0",','            Kind::MrsCtr => "mrs_ctr_el0",\n            Kind::X18 => "x18",'],
  ['        _ => None,\n    }\n}\n\n// ---- encoders','        _ if x18::classify(w) => Some((Kind::X18, w)),\n        _ => None,\n    }\n}\n\n// ---- encoders'],
  ['    brk(BRK_TAG | ((kind as u32) << 5) | rt)',`    let rt = if kind == Kind::X18 {
        x18::terminal(rt).expect("ordinary register sites require executable storage") | 16
    } else { rt };
    brk(BRK_TAG | ((kind as u32) << 5) | rt)`],
  ['    stub_body(kind, 0, 0).len() + 1','    if kind == Kind::X18 { x18::MAX_WORDS } else { stub_body(kind, 0, 0).len().max(stub_body(kind,18,0).len()) + 1 }'],
  ['    match kind {\n        // Saves','    match kind {\n        Kind::X18 => unreachable!("register emission requires source and veneer addresses"),\n        // Saves'],
  ['        Kind::MrsCtr if rt == 31 =>',`        Kind::MrsCtr if rt == 18 => vec![
            stp_pre(16,17,SP,-16), MRS_TPIDRRO_EL0|16,
            movz(17,ctr_el0&0xffff,0), movk(17,ctr_el0>>16,16),
            str_uimm(17,16,slot::SCS), ldp_post(16,17,SP,16)],
        Kind::MrsCtr if rt == 31 =>`],
  ['    let mut words = stub_body(kind, rt, ctr_el0);',`    if kind == Kind::X18 {
        encode_b(site,stub)?;
        return x18::emit(rt,site,stub,ctr_el0);
    }
    let mut words = stub_body(kind, rt, ctr_el0);`],
 ]);
 prepareAimThreadPointerEmitter(path.join(crate,'src/a64.rs'));
 // Keep derive on the enum, not on the imported source module.
 edit('src/a64.rs', [['#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]\n#[path = "x18.rs"]\nmod x18;\n\npub enum Kind {',
 '#[path = "x18.rs"]\nmod x18;\n\n#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]\npub enum Kind {']]);
 edit('src/xlate/mod.rs',[
  ['pub const VERSION: u32 = 2;','pub const VERSION: u32 = 19;'],
  ['pub sites: [usize; 6]','pub sites: [usize; 7]'],
  ['            None => {\n                report.brk_fallback += 1;',`            None if s.kind == Kind::X18 => {
                return done(Outcome::Unsupported(format!("register veneer requires load-time placement at {:#x}",s.vaddr)),report);
            }
            None => {\n                report.brk_fallback += 1;`],
 ]);
 // The original ELF metadata/range owner is shared with the native loader.
 // Both cached translation and live mmap call this exact analyze function.
 {
  const file='src/xlate/mod.rs',source=fs.readFileSync(path.join(crate,file),'utf8');
  const begin=source.indexOf('    let in_exec = |a: u64, e: u64| -> Vec<(u64, u64)> {');
  const end=source.indexOf('    let mut sites = Vec::new();',begin);
  if(begin<0||end<0)throw Error('AIM original ELF discovery boundary changed');
  edit(file,[[source.slice(begin,end),`    let state=a64::elf_instruction_evidence(b);
    let method=Method::SectionsAndEvidence;
    let is_oat=elf.symbols().iter().any(|s|!s.full&&s.name==b"oatdata");

`],
   ['                if !code.contains(v) {','                let flags=state[off as usize/4];\n                if flags==0 {'],
   ['                } else if data.contains(v) {','                } else if flags&2!=0 {'],
   ['ambiguous: method != Method::SectionsAndEvidence || !evidence.contains(v),','ambiguous: flags&4==0,']]);
 }
 edit('src/bin/linux-translate.rs',[['sites: [usize; 6]','sites: [usize; 7]']]);
 edit('src/patch.rs',[
  ['    pub ctr: usize,','    pub ctr: usize,\n    pub x18: usize,'],
  ['        self.ctr += o.ctr;','        self.ctr += o.ctr;\n        self.x18 += o.x18;'],
  ['self.svc + self.mrs_tp + self.msr_tp + self.scs + self.ctr','self.svc + self.mrs_tp + self.msr_tp + self.scs + self.ctr + self.x18'],
  ['            Kind::MrsCtr => self.ctr += 1,','            Kind::MrsCtr => self.ctr += 1,\n            Kind::X18 => self.x18 += 1,'],
  ['if word == a64::brk_fallback(kind, rt) {\n        return true;\n    }','    if (kind!=Kind::X18 || a64::x18_terminal(rt).is_some()) && word==a64::brk_fallback(kind,rt){return true;}'],
  ['            Kind::MrsTp => set(ss, rt, slot(a64::slot::TP).read()),',`            Kind::X18 => {
                let target=slot(a64::slot::SCS).read();
                if rt & 15 == 1 {
                    ss.lr=if rt & 16 != 0 {ss.pc+4} else {
                        ss.pc.wrapping_add(std::ptr::read_unaligned((ss.pc+4)as*const u64))
                    };
                }
                ss.pc=target.wrapping_sub(4);
            }
            Kind::MrsTp if rt==18 => slot(a64::slot::SCS).write(slot(a64::slot::TP).read()),
            Kind::MsrTp if rt==18 => slot(a64::slot::TP).write(slot(a64::slot::SCS).read()),
            Kind::MrsCtr if rt==18 => slot(a64::slot::SCS).write(a64::host_ctr_el0()as u64),
            Kind::MrsTp => set(ss, rt, slot(a64::slot::TP).read()),`],
 ]);
 // A general instruction needs executable storage. Preserve original mmap /
 // loader error handling if the owned island allocator cannot supply it;
 // never publish an untranslated instruction or a fabricated trap result.
 edit('src/patch.rs',[
  ['use std::sync::Mutex;', 'use std::{sync::Mutex, io};'],
  ['    use_islands: bool,\n) -> PatchStats {', '    use_islands: bool,\n) -> io::Result<PatchStats> {'],
  ['let (words, stats) = prepare_rewrite_sites(sites, lo, hi, use_islands);\n    write_words(&words);\n    stats',
   'let(words,stats)=prepare_rewrite_sites(sites,lo,hi,use_islands)?;\n    write_words(&words);Ok(stats)'],
  [') -> (Vec<(u64, u32)>, PatchStats) {', ') -> io::Result<(Vec<(u64,u32)>,PatchStats)> {'],
  ['        let word = word.unwrap_or_else(|| {', `        if word.is_none() && kind==Kind::X18 && a64::x18_terminal(rt).is_none() {
            return Err(io::Error::from_raw_os_error(if placed.is_none(){libc::ENOMEM}else{libc::ENOEXEC}));
        }
        let word = word.unwrap_or_else(|| {`],
  ['    (site_words, stats)\n}', '    Ok((site_words,stats))\n}'],
  ['pub fn rewrite_region(addr: u64, len: u64) -> PatchStats {', 'pub fn rewrite_region(addr: u64, len: u64) -> io::Result<PatchStats> {'],
  ['pub fn rewrite_region_with(addr: u64, len: u64, use_islands: bool) -> PatchStats {', 'pub fn rewrite_region_with(addr: u64, len: u64, use_islands: bool) -> io::Result<PatchStats> {'],
 ]);
 edit('src/a64.rs',[['mod x18;', 'mod x18;\npub(crate) use x18::{terminal as x18_terminal,resume_stack as x18_resume_stack,evidence as instruction_evidence,elf_evidence as elf_instruction_evidence};']]);
 edit('src/sys/signal.rs',[[
  '        if sig == libc::SIGTRAP && crate::patch::handle_brk(uc as *mut libc::ucontext_t) {',
  `        if sig == libc::SIGILL && !info.is_null() && (*info).si_code > 0 &&
            (*info).si_code < 0x10000 && crate::a64::x18_resume_stack(uc) { return; }
        if sig == libc::SIGTRAP && crate::patch::handle_brk(uc as *mut libc::ucontext_t) {`
 ]]);
 edit('src/patch.rs',[
  ['    let mut site = start;',`    let words=((addr+len-start)/4)as usize;
    let bytes=unsafe {std::slice::from_raw_parts(start as *const u8,words*4)};
    let mut evidence=vec![0;words];a64::instruction_evidence(bytes,start,&[start],&mut evidence);
    let mut site = start;`],
  ['        if let Some((kind, rt)) = a64::classify(insn) {',
   '        if evidence[((site-start)/4)as usize]&2==0 && let Some((kind, rt)) = a64::classify(insn) {'],
 ]);
 edit('src/a64.rs',[[
  '        for kind in Kind::ALL {\n            for rt in 0..31 {',
  '        for kind in Kind::ALL.into_iter().filter(|kind|*kind!=Kind::X18) {\n            for rt in 0..31 {'
 ]]);
 edit('src/loader.rs',[
  ['patch::rewrite_sites(&sites, bias + h.lo, bias + h.hi, true);', 'patch::rewrite_sites(&sites, bias + h.lo, bias + h.hi, true).map_err(|error|format!("{name}: executable register translation: {error}"))?;'],
  ['stats.add(&patch::rewrite_region(bias + p.p_vaddr, p.p_filesz));', 'stats.add(&patch::rewrite_region(bias + p.p_vaddr, p.p_filesz).map_err(|error|format!("{name}: executable register translation: {error}"))?);'],
 ]);
 edit('src/sys/verity_pager.rs',[
  ['crate::patch::prepare_rewrite_sites(&targets, lo, hi, true);','crate::patch::prepare_rewrite_sites(&targets,lo,hi,true)?;'],
 ]);
 edit('src/sys/mem.rs',[
  ['patch::rewrite_sites(&abs, b, b + len, true);', 'patch::rewrite_sites(&abs, b, b + len, true).map_err(|error|-(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64))?;'],
  ['ExecSource::LoadTime(None) => patch::rewrite_region(b, len),', 'ExecSource::LoadTime(None) => patch::rewrite_region(b, len).map_err(|error|-(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64))?,'],
  ['                trace_stats("rewrote", b, len, &patch::rewrite_region(b, len));', `                let stats=match patch::rewrite_region(b,len){Ok(stats)=>stats,Err(error)=>{
                    window::unmap(b,len);return -(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64);
                }};
                trace_stats("rewrote", b, len, &stats);`],
  ['                trace_stats("rewrote", lo, hi - lo, &patch::rewrite_region(lo, hi - lo));', `                let stats=match patch::rewrite_region(lo,hi-lo){Ok(stats)=>stats,Err(error)=>{
                    host_mprotect(lo,hi-lo,cur_prot);
                    return -(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64);
                }};
                trace_stats("rewrote", lo, hi - lo, &stats);`],
 ]);
 // Keep original component tests on the same fallible publication API.
 for(const [file,from,to]of[
  ['src/xlate/tests.rs','assert_eq!(r.sites, [1, 1, 1, 1, 1, 1]);','assert_eq!(r.sites, [1, 1, 1, 1, 1, 1, 0]);'],
  ['src/xlate/exec_tests.rs','assert_eq!(rep.sites, [1, 1, 1, 1, 1, 1]);','assert_eq!(rep.sites, [1, 1, 1, 1, 1, 1, 0]);'],
  ['examples/boundary_probe.rs','patch::rewrite_region_with(p as u64, len as u64, islands);','patch::rewrite_region_with(p as u64, len as u64, islands).unwrap();'],
  ['tests/hostcall.rs','patch::rewrite_region_with(p as u64, len as u64, islands);','patch::rewrite_region_with(p as u64, len as u64, islands).unwrap();'],
  ['tests/boundary.rs','patch::rewrite_region_with(p as u64, len as u64, islands);','patch::rewrite_region_with(p as u64, len as u64, islands).unwrap();'],
  ['tests/boundary.rs','patch::rewrite_region(code, 64);','patch::rewrite_region(code, 64).unwrap();'],
  ['tests/mremap.rs','patch::rewrite_region_with(p, (e as usize - s as usize) as u64, true);','patch::rewrite_region_with(p, (e as usize - s as usize) as u64, true).unwrap();'],
 ])edit(file,[[from,to]]);
}
