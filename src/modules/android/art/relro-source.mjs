import fs from 'node:fs';
import path from 'node:path';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';

/** Adapt the original loader's RELRO request to the physical page size. */
export function prepareRelro(aim){
 const root=path.join(aim,'crates/aim-linux-abi/src/sys');
 copyChanged(path.join(import.meta.dirname,'native/relro.rs'),path.join(root,'relro.rs'));
 const file=path.join(root,'mem.rs');let source=fs.readFileSync(file,'utf8');
 const from='    mprotect_owned(a)\n}\nfn mprotect_owned(a:[u64;6])->i64 {';
 if(source.split(from).length!==2)throw Error('AIM loader protection boundary changed');
 source=source.replace(from,'    let a=if let Some(adapted)=relro::request(a) {\n        if adapted[1]==0 {return 0;}\n        adapted\n    } else {a};\n    mprotect_owned(a)\n}\n#[path="relro.rs"]\nmod relro;\nfn mprotect_owned(a:[u64;6])->i64 {');
 writeChanged(file,source);
}
