import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';

const sources=[
  {
    "path": "crates/aim-linux-abi/src/sys/exec.rs",
    "beforeSha256": "cfd5928e13a6a2f2d09da81803ff2c7310f613c8cd5397683ebd64081f1f28d0",
    "sha256": "29ddd04d663d5559ce19cf400e4018bbc221a1258fee3c553ef764826dc3c991"
  },
  {
    "path": "crates/aim-linux-abi/src/sys/exec_fd_receipt.rs",
    "beforeSha256": "5f494fab4bf9a76e659dbdd48887a352258b2500a390f354e3c6ea62d2a77961",
    "sha256": "34eff27cc49f99c227f618067396e77becbbf7f0eb9cda51dd2f0d08c7423291"
  },
  {
    "path": "crates/aim-linux-abi/src/sys/fdtab.rs",
    "beforeSha256": "7be2efd235c8c6453cbe55673a7f20985d12f96beec758d343b946a68e6abebe",
    "sha256": "86b9374b040821fd524b745eb3c8ae46654116b0edf61b56e741a1737fb5a53b"
  }
];
const hash=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');

/** Preserve typed CLOEXEC retirement, retain its regular close owners and restore temporary exec flags
 * before releasing descriptor admission after a failed host exec.
 * Apply last, after the complete owned Linux ABI projection is prepared. */
export function prepareExecQueueRetirement(output){
 const current=sources.map(row=>hash(path.join(output,row.path)));
 if(sources.every((row,index)=>current[index]===row.sha256))return;
 for(const [index,row] of sources.entries())if(current[index]!==row.beforeSha256)throw Error('Linux exec source changed: '+row.path);
 execFileSync('git',['apply','--unsafe-paths',path.join(import.meta.dirname,'adapters/aim-exec-queue.patch')],{cwd:output,stdio:'inherit',env:{...process.env,GIT_CEILING_DIRECTORIES:path.dirname(output)}});
 for(const row of sources)if(hash(path.join(output,row.path))!==row.sha256)throw Error('Linux exec adaptation did not match: '+row.path);
}
