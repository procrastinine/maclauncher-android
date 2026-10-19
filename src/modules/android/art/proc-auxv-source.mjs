import fs from 'node:fs';
import path from 'node:path';
import {writeChanged} from '../native/upstream/source-files.mjs';

/** procfs exposes the exact guest ELF auxiliary vector already owned by the
 * loader. Save it independently of writable stack memory, as Linux does. */
export function prepareProcAuxv(aim){
 const root=path.join(aim,'crates/aim-linux-abi/src');
 const edit=(name,changes)=>{
  const file=path.join(root,name);let source=fs.readFileSync(file,'utf8');
  for(const[from,to]of changes){
   if(source.split(from).length!==2)throw Error('AIM proc auxiliary-vector boundary changed: '+name);
   source=source.replace(from,to);
  }
  writeChanged(file,source);
 };
 edit('loader.rs',[[
  '    for (k, v) in auxv {',
  '    crate::sys::note_auxv(&auxv);\n    for (k, v) in auxv {'
 ]]);
 edit('sys/mod.rs',[[
  'pub use procfs::{StackInfo, note_stack};',
  'pub use procfs::{StackInfo, note_stack, note_auxv};'
 ]]);
 edit('sys/procfs.rs',[
  ['static STACK: RwLock<Option<StackInfo>> = RwLock::new(None);',`static STACK: RwLock<Option<StackInfo>> = RwLock::new(None);

/// Linux mm->saved_auxv: captured at exec, never reread from writable argv memory.
static AUXV: RwLock<Vec<u8>> = RwLock::new(Vec::new());

pub fn note_auxv(vector: &[(u64, u64)]) {
    let mut bytes = Vec::with_capacity(vector.len() * 16);
    for &(key, value) in vector {
        bytes.extend_from_slice(&key.to_le_bytes());
        bytes.extend_from_slice(&value.to_le_bytes());
    }
    *AUXV.write().unwrap_or_else(|error| error.into_inner()) = bytes;
}

pub(super) fn saved_auxv() -> Vec<u8> {
    AUXV.read().unwrap_or_else(|error| error.into_inner()).clone()
}`],
  ['    POOL.drain();','    POOL.drain();\n    w.bytes(&saved_auxv());'],
  ['pub(super) fn fork_restore(r: &mut super::fork_state::Reader) {',`pub(super) fn fork_restore(r: &mut super::fork_state::Reader) {
    *AUXV.write().unwrap_or_else(|error| error.into_inner()) = r.bytes();`],
  ['    ("attr", dir::DT_DIR),','    ("attr", dir::DT_DIR),\n    ("auxv", dir::DT_REG),'],
  ['        "cmdline" => Node::File(cmdline(p)),',`        "auxv" if me => Node::File(saved_auxv()),
        "auxv" => Node::File(super::procrec::auxv(p)?),
        "cmdline" => Node::File(cmdline(p)),`],
 ]);
 edit('sys/procrec.rs',[
  ['    _pad: u64,','    auxv_len: AtomicU64,'],
  ['    let strings: &[u8] = if hi > lo {',`    // The table-to-string gap stores the immutable per-exec auxiliary vector.
    let auxv = super::procfs::saved_auxv();
    if auxv.len() as u64 > STRINGS - TABLE as u64 { return; }
    let strings: &[u8] = if hi > lo {`],
  ['        || f.write_all_at(strings, STRINGS).is_err()',`        || f.write_all_at(strings, STRINGS).is_err()
        || f.write_all_at(&auxv, TABLE as u64).is_err()`],
  ['    let h = &t.header;','    let h = &t.header;\n    h.auxv_len.store(auxv.len() as u64, Relaxed);'],
  ['/// The command line of process `pid`, read from its string pages as',`/// The immutable auxiliary vector of another guest process.
pub fn auxv(pid: i32) -> Option<Vec<u8>> {
    let file = open(pid)?;
    let header = read_at(&file, 0, HEADER);
    let len = u64_at(&header, 56);
    if len % 16 != 0 || len > STRINGS - TABLE as u64 { return None; }
    let bytes = read_at(&file, TABLE as u64, len as usize);
    (bytes.len() == len as usize).then_some(bytes)
}

/// The command line of process \`pid\`, read from its string pages as`],
 ]);
}
