import fs from 'node:fs';
import path from 'node:path';
import {writeChanged} from '../native/upstream/source-files.mjs';

/** Keep the guest's original ELF file view while sparse execution owns only
 * rewritten instruction pages. Translation metadata remains available to the
 * existing executable-source and authenticated derivation owners. Apply after
 * prepareOriginalFiles; the execution owner supplies initial_execution::enabled.
 */
export function prepareInitialFiles(aim){
 const file=path.join(aim,'crates/aim-linux-abi/src/xrt.rs');
 let source=fs.readFileSync(file,'utf8');
 const edits=[
  [`//!   translated file ([\`on_open\`]). The guest linker then reads the`,
   `//!   translated file ([\`on_open\`]) unless original instruction execution owns
//!   the mapping; that mode retains the real original descriptor. Otherwise the guest linker reads the`],
  [`    let fd = open_published(&path, guest, host_flags & libc::O_CLOEXEC, Path::new(std::ffi::OsStr::from_bytes(host.to_bytes())), &st)?;`,
   `    if crate::sys::initial_execution::enabled(){
        // Retain the cache binding and HasTranslation state, but let open(2)
        // return the real original description, including its headers and stat.
        let original=Path::new(std::ffi::OsStr::from_bytes(host.to_bytes()));
        let _=original_files::bind_paths(original,&st,&path);
        return None;
    }
    let fd = open_published(&path, guest, host_flags & libc::O_CLOEXEC, Path::new(std::ffi::OsStr::from_bytes(host.to_bytes())), &st)?;`],
  [`    let Some(nfd) = open_published(&path, guest, libc::O_CLOEXEC, Path::new(std::ffi::OsStr::from_bytes(host.to_bytes())), &st) else {`,
   `    if crate::sys::initial_execution::enabled(){
        // The guest already owns the original fd. Cache identity is retained
        // privately; executable-source selection still consumes original sites.
        let original=Path::new(std::ffi::OsStr::from_bytes(host.to_bytes()));
        let _=original_files::bind_paths(original,&st,&path);
        return;
    }
    let Some(nfd) = open_published(&path, guest, libc::O_CLOEXEC, Path::new(std::ffi::OsStr::from_bytes(host.to_bytes())), &st) else {`],
  [`        Some(FileState::HasTranslation(p)) => if original_files::bind_paths(host,&st,&p).is_ok(){LoaderSource::File(p, "translation cache")}else{LoaderSource::LoadTime},`,
   `        Some(FileState::HasTranslation(p)) => if original_files::bind_paths(host,&st,&p).is_ok(){
            if crate::sys::initial_execution::enabled(){
                set_state(st,FileState::HasTranslation(p));
                LoaderSource::File(host.to_path_buf(),"original instruction view")
            }else{LoaderSource::File(p,"translation cache")}
        }else{LoaderSource::LoadTime},`],
 ];
 for(const[before,after]of edits){
  if(source.split(before).length!==2)throw Error('Original instruction file-view boundary changed: '+before.slice(0,100));
  source=source.replace(before,after);
 }
 writeChanged(file,source);
}
