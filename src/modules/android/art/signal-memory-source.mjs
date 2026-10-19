import fs from 'node:fs';
import path from 'node:path';
import {writeChanged} from '../native/upstream/source-files.mjs';

/** Linux user pointers use the original fault-contained kernel copy owner. */
export function prepareSignalUserMemory(aim){
 const file=path.join(aim,'crates/aim-linux-abi/src/sys/signal.rs');
 let source=fs.readFileSync(file,'utf8');
 const begin=source.indexOf('pub fn rt_sigprocmask(a: [u64; 6]) -> i64 {');
 const end=source.indexOf('\npub fn rt_sigpending(',begin);
 if(begin<0||end<0)throw Error('Original Linux signal-mask boundary changed');
 let body=source.slice(begin,end);
 const edits=[
  ['        // SAFETY: guest sigset.\n        let s = unsafe { (set as *const u64).read_unaligned() };',
   '        let bytes = match super::user_memory::read_exact(set, 8) {\n            Ok(bytes) => bytes, Err(error) => return -(error as i64),\n        };\n        let s = u64::from_ne_bytes(bytes.try_into().unwrap());'],
  ['        // SAFETY: guest sigset.\n        unsafe { (oset as *mut u64).write_unaligned(old) };',
   '        if let Err(error) = super::user_memory::write_exact(oset, &old.to_ne_bytes()) {\n            return -(error as i64);\n        }'],
 ];
 for(const [before,after] of edits){if(body.split(before).length!==2)throw Error('Original Linux signal-mask user-copy boundary changed');body=body.replace(before,after);}
 writeChanged(file,source.slice(0,begin)+body+source.slice(end));
}
