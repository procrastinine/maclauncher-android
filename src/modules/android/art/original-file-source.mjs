import fs from 'node:fs';import path from 'node:path';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';
/** Bind AOT cache files to their original source identity for mutable mappings. */
export function prepareOriginalFiles(aim){
 const root=path.join(aim,'crates/aim-linux-abi/src');
 const edit=(name,changes)=>{const file=path.join(root,name);let text=fs.readFileSync(file,'utf8');for(const[before,after]of changes){if(text.split(before).length!==2)throw Error('Original file ownership boundary changed: '+name+' '+before.slice(0,90));text=text.replace(before,after);}writeChanged(file,text);};
 copyChanged(path.join(import.meta.dirname,'native/original-files.rs'),path.join(root,'original-files.rs'));
 edit('xrt.rs',[
  ['static RT: OnceLock<Runtime> = OnceLock::new();','#[path="original-files.rs"]\nmod original_files;\npub use original_files::{restore_original_mapping,has_original_mapping,reload_original_mapping};\n\nstatic RT: OnceLock<Runtime> = OnceLock::new();'],
  ['fn open_published(path: &Path, guest: &str, cloexec: i32) -> Option<i32> {','fn open_published(path: &Path, guest: &str, cloexec: i32, original: &Path, original_stat: &FileStat) -> Option<i32> {'],
  ['    if let Ok((tst, _)) = FileStat::of_fd(fd) {','    if original_files::bind(original,original_stat,fd,path).is_err(){unsafe{libc::close(fd);}return None;}\n    if let Ok((tst, _)) = FileStat::of_fd(fd) {'],
  ['open_published(&path, guest, host_flags & libc::O_CLOEXEC)?','open_published(&path, guest, host_flags & libc::O_CLOEXEC, Path::new(std::ffi::OsStr::from_bytes(host.to_bytes())), &st)?'],
  ['open_published(&path, guest, libc::O_CLOEXEC) else','open_published(&path, guest, libc::O_CLOEXEC, Path::new(std::ffi::OsStr::from_bytes(host.to_bytes())), &st) else'],
  ['pub fn note_mapped(host: &Path, guest: &str) {','pub fn note_mapped(host: &Path, guest: &str, original: &Path, original_stat: &FileStat) {\n    if in_cache(host){let _=original_files::bind_paths(original,original_stat,host);}'],
  ['Some(FileState::HasTranslation(p)) => LoaderSource::File(p, "translation cache"),','Some(FileState::HasTranslation(p)) => if original_files::bind_paths(host,&st,&p).is_ok(){LoaderSource::File(p, "translation cache")}else{LoaderSource::LoadTime},'],
  ['pub(crate) fn fork_save(w: &mut crate::sys::fork_state::Writer) {','pub(crate) fn fork_save(w: &mut crate::sys::fork_state::Writer) {\n    original_files::fork_save(w);'],
  ['pub(crate) fn fork_restore(r: &mut crate::sys::fork_state::Reader) {','pub(crate) fn fork_restore(r: &mut crate::sys::fork_state::Reader) {\n    original_files::fork_restore(r);'],
 ]);
 edit('loader.rs', [
  ['        let h = read_headers(&file, name)?;','        let original_stat=crate::cache::FileStat::of_fd(original.as_raw_fd()).map_err(|error|format!("{name}: original identity: {error}"))?.0;\n        let h = read_headers(&file, name)?;'],
  ['xrt::note_mapped(&mapped, name);','xrt::note_mapped(&mapped, name, Path::new(path), &original_stat);'],
 ]);
 edit('sys/mod.rs', [['pub use copies::note as note_copy;', 'pub use copies::note as note_copy;\npub(crate) use copies::original_file_regions as original_file_copy_regions;\npub(crate) use vmmap::regions as original_file_mapping_regions;\n#[cfg(test)] pub(crate) use copies::forget as forget_original_file_test_copy;']]);
 edit('sys/copies.rs', [['/// mremap moved `[old, old+len)` to `new`.','/// Original copy extents overlapping a currently leased VM range.\npub(crate) fn original_file_regions(lo:u64,hi:u64)->Vec<(u64,Copy)>{\n    COPIES.lock().unwrap().range(..hi).filter(|(start,copy)|**start<hi&&copy.end>lo).map(|(start,copy)|(*start,copy.clone())).collect()\n}\n\n/// mremap moved `[old, old+len)` to `new`.']]);
}
