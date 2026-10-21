import fs from 'node:fs';
import path from 'node:path';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';

/** Preserve the original fault owner and add bounded fatal-report context. */
export function prepareFaultDiagnostics(aim){
 const root=path.join(aim,'crates/aim-linux-abi/src');
 const edit=(name,changes)=>{const file=path.join(root,name);let source=fs.readFileSync(file,'utf8');
  for(const[from,to]of changes){if(source.split(from).length!==2)throw Error('AIM fault-report boundary changed: '+name+' '+from.slice(0,60));source=source.replace(from,to);}
  writeChanged(file,source);
 };
 copyChanged(path.join(import.meta.dirname,'native/fault-diagnostics.rs'),path.join(root,'fault-diagnostics.rs'));
 edit('sys/mod.rs',[
  ['        crate::diag!("{line} [pid={pid} tid={tid} call={call} enter]");',
   '        crate::diag!("{line} [pid={pid} tid={tid} call={call} enter] [pc={:#x} lr={:#x} fp={:#x}]",ctx.resume_pc().wrapping_sub(4),ctx.x[30],ctx.x[29]);\n        if nr==215 {crate::diag!("[linux] memory callers [pid={pid} tid={tid} call={call}] {}",crate::diag::trace_frame_returns(ctx.x[29]));}'],
 ]);
 const registers=`        for i in (0..29).step_by(4) {
            let mut line = String::from("[linux-abi]  ");
            for j in i..(i + 4).min(29) {
                line.push_str(&format!(" x{j:<2} {:#018x}", ss.x[j]));
            }
            crate::diag!("{line}");
        }`;
 edit('diag.rs',[
  ['pub unsafe fn report(sig: i32, info: *mut libc::siginfo_t, uc: *mut libc::c_void) {','pub unsafe fn report(sig: i32, info: *mut libc::siginfo_t, uc: *mut libc::c_void) {\n    let _errno = FaultReportErrno::save();\n    fault_process_identity();'],
  [registers,'        fault_registers(&ss.x);\n        fault_instructions(ss.pc);\n        report_live_guest_syscall();'],
  ['            "[linux-abi]   lr {:#x} ({}) sp {:#x}",\n            ss.lr,\n            describe(ss.lr),\n            ss.sp','            "[linux-abi]   lr {:#x} ({}) sp {:#x} fp {:#x}",\n            ss.lr,\n            describe(ss.lr),\n            ss.sp,\n            ss.fp'],
  ['    pub esr: u32,\n}','    pub esr: u32,\n    pub x: [u64; 29],\n    pub instruction: Option<u32>,\n}'],
  ['pub fn report_death(sig: i32) {','pub fn report_death(sig: i32) {\n    let _errno = FaultReportErrno::save();'],
  ['    let Some(f) = LAST_FAULT.with(|c| c.take()).filter(|f| f.sig == sig) else {\n        return;\n    };','    let Some(f) = LAST_FAULT.with(|c| c.take()).filter(|f| f.sig == sig) else {\n        report_software_abort(sig);\n        return;\n    };'],
  ['    crate::diag!(\n        "[linux-abi] fatal signal {sig} after','    fault_process_identity();\n    crate::diag!(\n        "[linux-abi] fatal signal {sig} after'],
  ['    frames(f.fp);','    fault_registers(&f.x);\n    if let Some(word) = f.instruction { crate::diag!("[linux-abi]   instruction at interruption {:#018x}: {word:08x}", f.pc); }\n    fault_instructions(f.pc);\n    frames(f.fp);'],
  ['        "[linux-abi]   lr {:#x} ({}) sp {:#x}",\n        f.lr,\n        describe(f.lr),\n        f.sp','        "[linux-abi]   lr {:#x} ({}) sp {:#x} fp {:#x}",\n        f.lr,\n        describe(f.lr),\n        f.sp,\n        f.fp'],
  ['/// Most frames of a frame-pointer walk.','include!("fault-diagnostics.rs");\n\n/// Most frames of a frame-pointer walk.'],
  ['fn frames(mut fp: u64) {',`fn frame_record(fp:u64)->Option<[u64;2]>{
    if fp==0||fp%16!=0{return None;}
    let mut rec=[0u64;2];let mut got=0u64;
    let result=unsafe{mach_vm_read_overwrite(mach_task_self_,fp,16,rec.as_mut_ptr()as u64,&mut got)};
    (result==0&&got==16&&rec[1]!=0).then_some(rec)
}
fn frames(mut fp: u64) {`],
  [`        let mut rec = [0u64; 2];
        let mut got = 0u64;
        // SAFETY: a fault-safe copy from our own task into a local.
        let kr = unsafe {
            mach_vm_read_overwrite(mach_task_self_, fp, 16, rec.as_mut_ptr() as u64, &mut got)
        };
        if fp == 0 || fp % 16 != 0 || kr != 0 || got != 16 || rec[1] == 0 {
            return;
        }`,'        let Some(rec)=frame_record(fp)else{return;};'],
 ]);
 edit('sys/signal.rs',[
  ['        Ok(th) => send_thread(&th, sender(sig, sigframe::SI_TKILL)),',
   '        Ok(th) => {\n            crate::diag::report_abort_request(sig, th.tid);\n            send_thread(&th, sender(sig, sigframe::SI_TKILL))\n        },'],
  ['    crate::diag::note_fault(crate::diag::Fault {','    let snapshot = crate::diag::Fault {'],
  ['        esr: f.esr as u32,\n','        esr: f.esr as u32,\n        x: { let mut x=m.x; x[18]=crate::context::guest_scs(); x },\n        instruction: crate::diag::interrupted_instruction(sig, m.pc),\n'],
  ['    });\n    let cpu = Cpu::from_mc(m);','    };\n    crate::diag::note_fault(snapshot);\n    crate::diag::report_handled_fault(&snapshot, f.info.code, th.tid);\n    let cpu = Cpu::from_mc(m);'],
  ['pub fn install_host_handlers() {','pub fn install_host_handlers() {\n    crate::diag::configure_fault_instruction_capture(std::env::var_os("MACLAUNCHER_ART_FAULT_INSTRUCTION").is_some_and(|value| value == "1"));\n    crate::diag::configure_abort_request_capture(std::env::var_os("MACLAUNCHER_ART_ABORT_CONTEXT").is_some_and(|value| value == "1"));'],
  ['    ONCE.call_once(|| {\n        forward(CARRIER, 0);','    ONCE.call_once(|| {\n        crate::diag::configure_handled_fault_capture(std::env::var_os("MACLAUNCHER_ART_HANDLED_FAULT_CONTEXT").is_some_and(|value| value == "1"));\n        forward(CARRIER, 0);'],
 ]);
}
