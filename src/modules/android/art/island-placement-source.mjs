import fs from 'node:fs';
import path from 'node:path';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';

/** A large reserved ELF can contain all three upstream placement hints.
 * Search real free VM gaps without replacing any existing guest mapping. */
export function prepareIslandPlacement(aim){
 const root=path.join(aim,'crates/aim-linux-abi/src'),file=path.join(root,'patch.rs');
 let source=fs.readFileSync(file,'utf8');
 const edit=(before,after)=>{
  if(source.split(before).length!==2)throw Error('Original instruction island placement boundary changed');
  source=source.replace(before,after);
 };
 copyChanged(path.join(import.meta.dirname,'native/island-placement.rs'),path.join(root,'island-placement.rs'));
 // Keep actual mapping capacity with its owner. Small ELF alignment gaps can
 // hold a page of stubs even when they cannot hold the default 256 KiB island.
 edit('    rw: u64,\n    used: u64,', '    rw: u64,\n    size: u64,\n    used: u64,');
 const begin=source.indexOf('fn new_island(hint: u64) -> Option<Island> {');
 const end=source.indexOf('/// execve in place:',begin);
 if(begin<0||end<0)throw Error('Original instruction island allocation boundary changed');
 const allocation=source.slice(begin,end).replace('fn new_island(hint: u64)', 'fn new_island_sized(hint: u64, size: u64)')
  .replaceAll('ISLAND_SIZE','size').replace('                used: 0,','                size,\n                used: 0,');
 source=source.slice(0,begin)+'fn new_island(hint: u64) -> Option<Island> { new_island_sized(hint, ISLAND_SIZE) }\n\n'+allocation+source.slice(end);
 edit('libc::munmap(isl.rw as *mut _, ISLAND_SIZE as usize);\n            libc::munmap(isl.rx as *mut _, ISLAND_SIZE as usize);',
  'libc::munmap(isl.rw as *mut _, isl.size as usize);\n            libc::munmap(isl.rx as *mut _, isl.size as usize);');
 edit('        w.u64(isl.used);','        w.u64(isl.used);\n        w.u64(isl.size);');
 edit('    let saved = r.seq(|r| (r.u64(), r.u64()));','    let saved = r.seq(|r| (r.u64(), r.u64(), r.u64()));');
 const restoreBegin=source.indexOf('pub(crate) fn fork_restore('),restoreEnd=source.indexOf('/// Reserve `bytes`',restoreBegin);
 if(restoreBegin<0||restoreEnd<0)throw Error('Original instruction island restore boundary changed');
 const restore=source.slice(restoreBegin,restoreEnd).replace('for (rx, used) in saved','for (rx, used, size) in saved')
  .replaceAll('ISLAND_SIZE','size').replace('Island { rx, rw, used }','Island { rx, rw, size, used }');
 source=source.slice(0,restoreBegin)+restore+source.slice(restoreEnd);
 edit('isl.used + bytes <= ISLAND_SIZE','isl.used + bytes <= isl.size');
 edit('libc::munmap(isl.rx as *mut _, ISLAND_SIZE as usize);\n                libc::munmap(isl.rw as *mut _, ISLAND_SIZE as usize);',
  'libc::munmap(isl.rx as *mut _, isl.size as usize);\n                libc::munmap(isl.rw as *mut _, isl.size as usize);');
 edit('/// Reserve `bytes` of stub space reachable from `site`; `[lo, hi)` is the',
  'include!("island-placement.rs");\n\n/// Reserve `bytes` of stub space reachable from `site`; `[lo, hi)` is the');
 edit('    None\n}\n\n#[derive(Default, Debug, Clone, Copy)]',
  '    alloc_stub_in_gaps(islands, site, bytes)\n}\n\n#[derive(Default, Debug, Clone, Copy)]');
 // Every region query may return an owned object-name port. The cold gap walk
 // borrows the bounds only, so retire that send right before returning them.
 edit('        pub fn mach_vm_deallocate(task: libc::mach_port_t, address: u64, size: u64) -> i32;',
  '        fn mach_port_deallocate(task: libc::mach_port_t, name: libc::mach_port_t) -> i32;\n        pub fn mach_vm_deallocate(task: libc::mach_port_t, address: u64, size: u64) -> i32;');
 edit('        (kr == 0).then_some((a, a + size, info[0], info[3] != 0))',
  '        if obj != 0 { unsafe { mach_port_deallocate(task(), obj); } }\n        (kr == 0).then_some((a, a + size, info[0], info[3] != 0))');
 writeChanged(file,source);
}
