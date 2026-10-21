import fs from 'node:fs';
import path from 'node:path';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';

/** Preserve original Bionic/sysconf entry points and the AIM memory owner.
 * Only its guest-visible budget/accounting and direct duplicate readers change. */
export function prepareMemoryBudget(aim){
 const edit=(relative,changes)=>{const file=path.join(aim,relative);let text=fs.readFileSync(file,'utf8');
  for(const [before,after] of changes){if(text.split(before).length!==2)throw Error('Original memory reporting boundary changed: '+relative);text=text.replace(before,after);}
  writeChanged(file,text);
 };
 const owner='crates/aim-host-memory/src';
 copyChanged(path.join(import.meta.dirname,'native/memory-budget.rs'),path.join(aim,owner,'memory-budget.rs'));
 const memory=path.join(aim,owner,'lib.rs'),original=fs.readFileSync(memory,'utf8');
 const readStart=original.indexOf('/// The Mac\'s pressure level and memory now.');
 const readEnd=original.indexOf('/// The write ends of the watchers\' pipes.',readStart);
 if(readStart<0||readEnd<0)throw Error('Original memory owner methods changed.');
 edit(owner+'/lib.rs',[
  ['use std::sync::{Mutex, OnceLock};','use std::sync::{Mutex, OnceLock};\n#[path="memory-budget.rs"] mod guest_budget;\npub use guest_budget::Snapshot as GuestMemory;'],
  [original.slice(readStart,readEnd),`/// The original host pressure level, with guest-budget memory in bytes.
pub fn read() -> Memory {
    let memory=guest_memory();
    Memory { level:pressure_level(),reserved:0,total:memory.total,free:memory.available,file:0 }
}

/// One current accounting sample for all Linux ABI memory reports.
pub fn guest_memory() -> GuestMemory { guest_budget::snapshot(total()) }
/// Live per-process headroom, bounded by the configured Android memory budget.
pub fn available() -> u64 { guest_memory().available }
/// Configured Android memory in bytes, capped by actual physical memory.
pub fn total() -> u64 { guest_budget::configured_total(sysctl::<u64>(c"hw.memsize").unwrap_or(0)) }

`],
  ['fn reads_the_macs_memory()','fn reads_the_guest_memory_budget()'],
  ['assert_eq!(Some(m.total), sysctl::<u64>(c"hw.memsize"));','assert_eq!(m.total, total());'],
  ['assert!(m.free > 0 && m.free < m.total, "{m:?}");','assert!(m.free <= m.total, "{m:?}");'],
  ['assert!(m.file < m.total, "{m:?}");','assert_eq!(m.file, 0, "no separately accounted guest page cache");'],
 ]);
 const procFile='crates/aim-linux-abi/src/sys/procfs.rs',proc=fs.readFileSync(path.join(aim,procFile),'utf8');
 const begin=proc.indexOf('fn meminfo() -> String {'),end=proc.indexOf('\nconst HWCAP_NAMES:',begin);
 if(begin<0||end<0)throw Error('Original proc memory report changed.');
 edit(procFile,[[proc.slice(begin,end),`fn meminfo() -> String {
    let memory=aim_host_memory::guest_memory();
    let kb=memory.total/1024;let free=memory.available/1024;
    // The guest has no separately accounted Linux kernel/cache categories.
    // Host-wide VM queues are not Android application memory.
    format!(
        "MemTotal:       {kb:>8} kB\\nMemFree:        {free:>8} kB\\nMemAvailable:   {free:>8} kB\\n\\
         Buffers:               0 kB\\nCached:                0 kB\\nSwapCached:            0 kB\\n\\
         Active:                0 kB\\nInactive:              0 kB\\nUnevictable:           0 kB\\n\\
         Mlocked:               0 kB\\nSwapTotal:             0 kB\\nSwapFree:              0 kB\\n\\
         Dirty:                 0 kB\\nWriteback:             0 kB\\nAnonPages:             0 kB\\n\\
         Mapped:                0 kB\\nShmem:                 0 kB\\nSlab:                  0 kB\\n\\
         SReclaimable:          0 kB\\nSUnreclaim:            0 kB\\nKernelStack:           0 kB\\n\\
         PageTables:            0 kB\\nCommitLimit:    {kb:>8} kB\\nCommitted_AS:          0 kB\\n\\
         VmallocTotal:   {:>8} kB\\nVmallocUsed:           0 kB\\nVmallocChunk:          0 kB\\n",
        1u64 << 37,
    )
}
`]]);
 edit('crates/aim-services/src/app_host/job_scheduler.rs',[[`    let mut bytes: u64 = 0;
    let mut len = std::mem::size_of::<u64>();
    // SAFETY: a u64 out-parameter of its size.
    unsafe {
        libc::sysctlbyname(c"hw.memsize".as_ptr(), (&mut bytes as *mut u64).cast(), &mut len, std::ptr::null_mut(), 0)
    };`,`    let bytes = aim_host_memory::total();`]]);
}
