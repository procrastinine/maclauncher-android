import fs from 'node:fs';
import path from 'node:path';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';

/** Bind original package files through AIM's existing per-process namespace. */
export function preparePackagePaths(aim){
 const edit=(relative,from,to)=>{const file=path.join(aim,relative),text=fs.readFileSync(file,'utf8');if(text.split(from).length!==2)throw Error('AIM package path boundary changed: '+relative);writeChanged(file,text.replace(from,to));};
 copyChanged(path.join(import.meta.dirname,'native/package-paths.rs'),path.join(aim,'crates/aim-linux-abi/src/sys/package_paths.rs'));
 edit('crates/aim-linux-abi/src/sys/mod.rs','pub(crate) mod user_memory;','pub(crate) mod user_memory;\npub(crate) mod package_paths;');
 edit('crates/aim-linux-abi/src/hostcall.rs','if func==12{return crate::sys::transport_peer(args,len); }','if func==12{return crate::sys::transport_peer(args,len); }\n        if func == 13 { return crate::sys::package_paths::bind(args, len); }');
}
