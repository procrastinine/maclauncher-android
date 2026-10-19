import fs from 'node:fs';
import path from 'node:path';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';

/** Preserve original per-inode ownership while importing application trees
 * without revisiting every descendant at each process start. */
export function prepareImportedStorage(aim){
 const edit=(relative,changes)=>{
  const file=path.join(aim,relative);let text=fs.readFileSync(file,'utf8');
  for(const [from,to]of changes){if(text.split(from).length!==2)throw Error('AIM imported inode boundary changed: '+relative);text=text.replace(from,to);}
  writeChanged(file,text);
 };
 copyChanged(path.join(import.meta.dirname,'native/imported-inodes.rs'),path.join(aim,'crates/aim-storage/src/imported_inodes.rs'));
 copyChanged(path.join(import.meta.dirname,'native/imported-attrs.rs'),path.join(aim,'crates/aim-linux-abi/src/sys/imported-attrs.rs'));
 copyChanged(path.join(import.meta.dirname,'native/imported-vfs.rs'),path.join(aim,'crates/aim-linux-abi/src/imported-vfs.rs'));
 edit('crates/aim-linux-abi/src/vfs.rs', [['pub fn writable_hosts() -> Option<Vec<PathBuf>> {','include!("imported-vfs.rs");\n\npub fn writable_hosts() -> Option<Vec<PathBuf>> {']]);
 edit('crates/aim-storage/src/lib.rs', [['pub mod guest_inode;','pub mod guest_inode;\npub mod imported_inodes;']]);
 edit('crates/aim-storage/src/guest_inode.rs',[
  ['    let p = c_path(path)?;\n    // SAFETY: NUL-terminated path and name, exact readable span.',`    write_attribute(path, &b, NAME, 0)
}

/// Shared physical attribute writer, including original temporary owner access.
pub(crate) fn write_attribute(path: &Path, b: &[u8], name: &[u8], flags: c_int) -> io::Result<()> {
    let p = c_path(path)?;
    // SAFETY: NUL-terminated path and name, exact readable span.`],
  ['            NAME.as_ptr().cast(),\n            b.as_ptr().cast(),','            name.as_ptr().cast(),\n            b.as_ptr().cast(),'],
  ['            XATTR_NOFOLLOW,\n        ) == 0','            XATTR_NOFOLLOW | flags,\n        ) == 0'],
 ]);
 edit('crates/aim-linux-abi/src/sys/attrs.rs',[
  ['fn inode_attrs(host: Host, st: &libc::stat) -> (Option<Attr>, Option<Attr>) {','include!("imported-attrs.rs");\n\nfn inode_attrs(host: Host, st: &libc::stat) -> (Option<Attr>, Option<Attr>) {'],
  ['    let guest = if vfs::on_read_only_root(st.st_dev) {','    let mut guest = if vfs::on_read_only_root(st.st_dev) {'],
  ['    let mut inodes = INODES.lock().unwrap();\n    let map = inodes.get_or_insert_with(HashMap::new);',`    if original.is_none() && guest.is_none_or(|a| !a.complete()) {
        match imported_attrs(host, st, false) {
            Ok(Some(a)) => guest = Some(a),
            Ok(None) => {},
            Err(error) => eprintln!("guest imported inode metadata failed: errno={error}"),
        }
    }
    let mut inodes = INODES.lock().unwrap();
    let map = inodes.get_or_insert_with(HashMap::new);`],
 ]);
 edit('crates/aim-linux-abi/src/sys/fsops.rs',[
  ['        (None, Some(old)) => {\n            errno::check', '        (None, Some(old)) => {\n            if let Err(error) = attrs::preserve_import(Host::Path(&old.host)) { return -(error as i64); }\n            errno::check'],
  ['    // SAFETY: host paths.\n    let r = unsafe { libc::renamex_np',`    if let Err(error) = attrs::preserve_import(Host::Path(&old.host)) { return -(error as i64); }
    if flags & RENAME_EXCHANGE != 0 {
        if let Err(error) = attrs::preserve_import(Host::Path(&new.host)) { return -(error as i64); }
    }
    // SAFETY: host paths.
    let r = unsafe { libc::renamex_np`],
 ]);
}
