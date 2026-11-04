import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';

const sources=[
  {
    "path": "crates/aim-linux-abi/src/sys/exec.rs",
    "beforeSha256": "8abe842bf046c5b27a0c8a0080d188a0104013cedbfce3657dfa2d9e5aecd024",
    "sha256": "d7bffeeecf5dd2f54cdf22e5d3f1793ae7283e6368124417ef1d6a8397e607b9"
  },
  {
    "path": "crates/aim-linux-abi/src/sys/exec_fd_receipt.rs",
    "beforeSha256": "ddf6a6e6fc7285c9e21d2612f2269f2ffe58cf9438a2ac8a63b9d323f6a09398",
    "sha256": "6ddadb4e27e01e08b785789fd0db2b28f6f09ff4c0f01e3ff658d18704bb1abf"
  },
  {
    "path": "crates/aim-linux-abi/src/sys/fdtab.rs",
    "beforeSha256": "298066e7e501ba8d40da6b14b25828cb16962b654bed5651ca377e7cc77b50a9",
    "sha256": "da18b387ddc3cbb70b54ac42755b88ee2e3bc0ae3c43b5b4bca7bf7fde7f3f9f"
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
