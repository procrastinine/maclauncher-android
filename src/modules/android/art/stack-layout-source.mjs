import fs from 'node:fs';
import path from 'node:path';
import {writeChanged} from '../native/upstream/source-files.mjs';

/** Linux ELF setup places the real initial stack at the architecture's stack
 * top. Reuse AIM's existing guest bounds, mapping and retirement owners. */
export function prepareInitialStackLayout(aim){
 const root=path.join(aim,'crates/aim-linux-abi/src');
 const edit=(file,from,to)=>{
  const target=path.join(root,file),source=fs.readFileSync(target,'utf8');
  if(source.split(from).length!==2)throw Error('AIM initial stack boundary changed: '+file);
  writeChanged(target,source.replace(from,to));
 };
 edit('sys/arena.rs','/// `mmap` placed in the guest range, searching from `hint`. `flags` must',`/// The initial ELF stack uses the top of the existing architecture window,
/// as Linux setup_arg_pages does. Ordinary mappings keep their own placement.
/// A non-fixed hint never replaces an occupied mapping; map retains its
/// ordinary bounds check, rollback and Linux error on allocation failure.
pub fn map_stack(len: u64, prot: i32) -> Result<u64, i64> {
    let base = HI.checked_sub(len).filter(|base| *base >= LO)
        .ok_or(-(ENOMEM as i64))?;
    map(base, len, prot, libc::MAP_PRIVATE | libc::MAP_ANON, -1, 0)
}

/// \`mmap\` placed in the guest range, searching from \`hint\`. \`flags\` must`);
 edit('sys/mod.rs','pub use arena::map_anon as map_guest_anon;',
  'pub use arena::map_anon as map_guest_anon;\npub use arena::map_stack as map_guest_stack;');
 edit('loader.rs','crate::sys::map_guest_anon(STACK_SIZE, libc::PROT_READ | libc::PROT_WRITE)',
  'crate::sys::map_guest_stack(STACK_SIZE, libc::PROT_READ | libc::PROT_WRITE)');
}
