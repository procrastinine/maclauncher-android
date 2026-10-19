import fs from 'node:fs';
import path from 'node:path';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';

/** The original process owns ELF verification, mappings and Linux state. */
export function prepareA64Execution(aim){
 const root=path.join(aim,'crates/aim-linux-abi');
 const edit=(name,changes)=>{const file=path.join(root,name);let text=fs.readFileSync(file,'utf8');for(const[from,to]of changes){if(text.split(from).length!==2)throw Error('A64 execution boundary changed: '+name+' '+from.slice(0,60));text=text.replace(from,to);}writeChanged(file,text);};
 copyChanged(path.join(import.meta.dirname,'native/a64-execution.rs'),path.join(root,'src/a64_execution.rs'));
 edit('src/lib.rs',[['pub mod context;','pub mod context;\npub mod a64_execution;']]);
 edit('build.rs',[
  ['fn main() {',`fn main() {
    println!("cargo:rustc-check-cfg=cfg(maclauncher_a64)");
    println!("cargo:rerun-if-env-changed=MACLAUNCHER_A64_ARCHIVES");
    if let Some(archives)=env::var_os("MACLAUNCHER_A64_ARCHIVES") {
        for archive in env::split_paths(&archives) {
            println!("cargo:rerun-if-changed={}",archive.display());
            println!("cargo:rustc-link-search=native={}",archive.parent().unwrap().display());
            let name=archive.file_name().unwrap().to_str().unwrap().strip_prefix("lib").unwrap().strip_suffix(".a").unwrap();
            println!("cargo:rustc-link-lib=static={name}");
        }
        println!("cargo:rustc-link-lib=c++");
        println!("cargo:rustc-cfg=maclauncher_a64");
    }`],
 ]);
 const branch=call=>'#[cfg(maclauncher_a64)]\n    if crate::a64_execution::enabled(){unsafe{'+call+'}}\n    ';
 edit('src/context.rs',[
  ['    unsafe { linux_abi_enter_guest(prepared.0 as *const _) }','    '+branch('crate::a64_execution::enter(prepared.0);')+'unsafe { linux_abi_enter_guest(prepared.0 as *const _) }'],
  ['    unsafe { linux_abi_resume(ctx as *const _) }','    '+branch('crate::a64_execution::enter(ctx);')+'unsafe { linux_abi_resume(ctx as *const _) }'],
  ['    unsafe { linux_abi_resume_trap() }','    '+branch('crate::a64_execution::enter(current_ctx());')+'unsafe { linux_abi_resume_trap() }'],
  ['pub unsafe extern "C" fn call_guest(function: u64, argument: u64) -> u64 {','pub unsafe extern "C" fn call_guest(function: u64, argument: u64) -> u64 {\n    #[cfg(maclauncher_a64)]\n    if crate::a64_execution::enabled(){return unsafe{crate::a64_execution::call_guest(function,argument)};}'],
 ]);
 edit('src/sys/mod.rs',[['mod signal;','mod signal;\n#[cfg(maclauncher_a64)]\npub(crate) use signal::{shadow_deliver,shadow_fault};']]);
 edit('src/sys/user_memory.rs',[
  ['pub(super) fn prepare_write','pub(crate) fn prepare_write'],
  ['/// Prepare a readable user range',`pub(crate) fn prepare_execute(address:u64,length:usize)->Result<(),Errno>{prepare(address,length,Access::Execute)}

/// Prepare a readable user range`]]);
 edit('src/sys/signal.rs',[
  ['            if th.sig.exact.swap(false, SeqCst) {\n                context::resume_trap();\n            }','            if th.sig.exact.swap(false, SeqCst) && !crate::a64_execution::enabled() {\n                context::resume_trap();\n            }'],
 ]);
 {
  const file=path.join(root,'src/sys/signal.rs');let text=fs.readFileSync(file,'utf8');
  const begin=text.indexOf('fn guest_fault('),start=text.indexOf('    let th = thread_of(ctx)?;',begin),end=text.indexOf('\n}\n',start);
  if(begin<0||start<0||end<0)throw Error('Original synchronous signal frame boundary changed');
  text=text.slice(0,start)+`    let cpu=Cpu::from_mc(m);
    match synchronous_entry(ctx,&cpu,f.info,f.esr,f.fault_address) {
        Ok(entry)=>{entry.to_mc(m);None},Err(sig)=>Some(sig),
    }`+text.slice(end);
  const marker='/// The \`rt_sigreturn\` trap: load the complete context into the host state.';
  const helper=`fn synchronous_entry(ctx:&GuestContext,cpu:&Cpu,info:Siginfo,esr:u64,address:u64)->Result<Entry,i32>{
    let sig=info.signo;let th=thread_of(ctx).ok_or(sig)?;let act=action(sig);let mask=th.sig.mask();
    if act.handler==SIG_DFL||act.handler==SIG_IGN||mask&bit(sig)!=0{return Err(sig);}
    let mut x=[0;29];x.copy_from_slice(&cpu.x[..29]);
    let snapshot=crate::diag::Fault{sig,pc:cpu.pc,lr:cpu.x[30],fp:cpu.x[29],sp:cpu.sp,addr:address,esr:esr as u32,x,instruction:crate::diag::interrupted_instruction(sig,cpu.pc)};
    crate::diag::note_fault(snapshot);
    crate::diag::report_handled_fault(&snapshot,info.code,th.tid);
    Ok(frame(th,cpu,&Taken{info,act},mask,esr,address))
}
#[cfg(maclauncher_a64)]
pub(crate) fn shadow_deliver(ctx:*mut GuestContext){linux_abi_deliver(ctx);}
#[cfg(maclauncher_a64)]
pub(crate) fn shadow_fault(ctx:*mut GuestContext,sig:i32,code:i32,address:u64){
    let ctx=unsafe{&mut *ctx};let cpu=Cpu::from_ctx(ctx);
    match synchronous_entry(ctx,&cpu,Siginfo::fault(sig,code,address),0,address){Ok(entry)=>entry.to_ctx(ctx),Err(sig)=>die(sig)}
}

`;
  if(text.split(marker).length!==2)throw Error('Original signal helper boundary changed');
  writeChanged(file,text.replace(marker,helper+marker));
 }
 edit('src/xrt.rs',[
  ['pub fn open_translated(host: &CStr, guest: &str, host_flags: i32) -> Option<i32> {','pub fn open_translated(host: &CStr, guest: &str, host_flags: i32) -> Option<i32> {\n    if crate::a64_execution::enabled(){return None;}'],
  ['pub fn on_open(fd: i32, host: &CStr, guest: &str, host_flags: i32) {','pub fn on_open(fd: i32, host: &CStr, guest: &str, host_flags: i32) {\n    if crate::a64_execution::enabled(){return;}'],
  ['pub fn exec_source(fd: i32, off: u64) -> ExecSource {','pub fn exec_source(fd: i32, off: u64) -> ExecSource {\n    if crate::a64_execution::enabled(){return ExecSource::Shared;}'],
  ['pub fn loader_source(host: &Path) -> LoaderSource {','pub fn loader_source(host: &Path) -> LoaderSource {\n    if crate::a64_execution::enabled(){return LoaderSource::File(host.to_path_buf(),"original A64 code");}'],
  ['pub fn is_shared_source(host: &Path) -> bool {','pub fn is_shared_source(host: &Path) -> bool {\n    if crate::a64_execution::enabled(){return true;}'],
 ]);
 edit('src/xlate/mod.rs',[
  ['    sites.dedup_by_key(|s| s.vaddr);','    sites.dedup_by_key(|s| s.vaddr);\n    if crate::a64_execution::enabled(){sites.clear();}'],
  ['        fips: fips::find(elf),','        fips: if crate::a64_execution::enabled(){None}else{fips::find(elf)},'],
 ]);
 edit('src/xrt_verified.rs',[
  ['    if offset.checked_add(length).ok_or(errno::EIO)? > source.len()? {','    let caches=if crate::a64_execution::enabled(){&[][..]}else{caches};\n    if offset.checked_add(length).ok_or(errno::EIO)? > source.len()? {'],
 ]);
 edit('src/patch.rs',[
  ['fn prepare_rewrite_sites_at(sites:&[(u64,Kind,u32)],lo:u64,hi:u64,use_islands:bool,stage:&str)->io::Result<(Vec<(u64,u32)>,PatchStats)> {','fn prepare_rewrite_sites_at(sites:&[(u64,Kind,u32)],lo:u64,hi:u64,use_islands:bool,stage:&str)->io::Result<(Vec<(u64,u32)>,PatchStats)> {\n    if crate::a64_execution::enabled(){return Ok((Vec::new(),PatchStats::default()));}'],
  ['pub(crate) fn prepare_region_at(addr:u64,bytes:&[u8],use_islands:bool,stage:&str)->io::Result<(Vec<(u64,u32)>,PatchStats)> {','pub(crate) fn prepare_region_at(addr:u64,bytes:&[u8],use_islands:bool,stage:&str)->io::Result<(Vec<(u64,u32)>,PatchStats)> {\n    if crate::a64_execution::enabled(){return Ok((Vec::new(),PatchStats::default()));}'],
 ]);
}
