import fs from 'node:fs';
import path from 'node:path';
import {writeChanged} from '../native/upstream/source-files.mjs';

/** Preserve the saved guest ELF auxiliary vector and process-owned descriptor
 * views below the original procfs and authenticated process-agent interfaces. */
export function prepareProcAuxv(aim){
 const root=path.join(aim,'crates/aim-linux-abi/src');
 const edit=(name,changes)=>{
  const file=path.join(root,name);let source=fs.readFileSync(file,'utf8');
  for(const[from,to]of changes){
   if(source.split(from).length!==2)throw Error('AIM procfs source boundary changed: '+name+' '+from.slice(0,60));
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
 // Process-owned publication, including low native slots, is the authority for
 // procfs and the authenticated agent's descriptor views (AIM 3136fe473714).
 edit('sys/procfs.rs',[
  ['pub(super) fn fd_link(fd: i32) -> Option<String> {\n    if !fdtab::is_hidden(fd) {if let Some(path)=fdtab::virtual_path(fd){return Some(path);}}\n    if fdtab::is_hidden(fd) {\n        return None;\n    }','pub(super) fn fd_link(fd: i32) -> Option<String> {\n    if !fdtab::visible(fd) { return None; }\n    if let Some(path)=fdtab::virtual_path(fd){return Some(path);}'],
  ['            fdtab::open_fds()\n                .into_iter()\n                .filter(|&fd| !fdtab::is_hidden(fd))','            super::fd_visibility::visible()\n                .into_iter()'],
  ['    if fd < 0 || fdtab::is_hidden(fd) { return Some(Err(ENOENT)); }','    if !fdtab::visible(fd) { return Some(Err(ENOENT)); }'],
  ['        if fdtab::is_hidden(n) {','        if !fdtab::visible(n) {'],
 ]);
 edit('sys/ptrace/agent.rs',[[
  '            let fds: Vec<i32> = fdtab::open_fds()\n                .into_iter()\n                .filter(|&fd| !fdtab::is_hidden(fd))\n                .collect();',
  '            let fds = crate::sys::fd_visibility::visible();'
 ]]);
 edit('sys/procfs.rs',[[
  '    #[test]\n    fn guest_argv_skips_linux_run_options() {',
  `    #[test]
    fn descriptor_views_follow_publication_and_reuse() {
        if crate::sys::fdtab::isolated_kernel_test("sys::procfs::tests::descriptor_views_follow_publication_and_reuse") { return; }
        use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};
        let (_view, root) = crate::vfs::test_view();
        crate::sys::fdtab::install_storage_registrar().unwrap();
        let internal = std::fs::File::open("/dev/null").unwrap();
        let private = internal.as_raw_fd();
        assert!(!crate::sys::fdtab::is_hidden(private));
        assert!(!crate::sys::fdtab::visible(private));
        let names = || match super::pid_node(super::pid(), "fd", None).unwrap() {
            super::Node::Dir(entries) => entries.into_iter().map(|entry| entry.name).collect::<Vec<_>>(),
            _ => panic!("descriptor directory missing"),
        };
        let absent = |fd: i32| {
            assert!(!names().contains(&fd.to_string().into_bytes()));
            assert!(super::fd_link(fd).is_none());
            let path = format!("/proc/self/fd/{fd}");
            assert_eq!(super::current_fd_link(&path), Some(Err(crate::errno::ENOENT)));
            assert_eq!(super::open(&path, 0, libc::O_RDONLY), Some(-(crate::errno::ENOENT as i64)));
        };
        absent(private);
        std::fs::write(root.join("data/proc-fd-owner"), b"owned").unwrap();
        let name = std::ffi::CString::new("/data/proc-fd-owner").unwrap();
        let fd = crate::sys::fs::openat([crate::vfs::LINUX_AT_FDCWD as u64, name.as_ptr() as u64, 0, 0, 0, 0]);
        assert!(fd >= 0);
        let alias = crate::sys::fs::dup([fd as u64, 0, 0, 0, 0, 0]);
        assert!(alias >= 0);
        for target in [fd as i32, alias as i32] {
            assert!(names().contains(&target.to_string().into_bytes()));
            assert!(super::fd_link(target).is_some());
            assert_eq!(super::current_fd_link(&format!("/proc/self/fd/{target}")), Some(Ok(target)));
        }
        assert_eq!(crate::sys::fs::close([fd as u64, 0, 0, 0, 0, 0]), 0);
        assert_eq!(unsafe { libc::dup2(private, fd as i32) }, fd as i32);
        let _reused = unsafe { OwnedFd::from_raw_fd(fd as i32) };
        absent(fd as i32);
        assert!(names().contains(&alias.to_string().into_bytes()));
        assert_eq!(crate::sys::fs::close([alias as u64, 0, 0, 0, 0, 0]), 0);
    }

    #[test]
    fn guest_argv_skips_linux_run_options() {`
 ]]);
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
