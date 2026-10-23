import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';

const sources=[
  {
    "path": "crates/aim-linux-abi/src/sys/exec.rs",
    "beforeSha256": "cfd5928e13a6a2f2d09da81803ff2c7310f613c8cd5397683ebd64081f1f28d0",
    "sha256": "807f632fc445c2ce93bbfdeb3323ed5daa27f6b3bad8c3c4c06dc8e30d893de7"
  },
  {
    "path": "crates/aim-linux-abi/src/sys/exec_fd_receipt.rs",
    "beforeSha256": "5f494fab4bf9a76e659dbdd48887a352258b2500a390f354e3c6ea62d2a77961",
    "sha256": "2a9e0fd27329f6efee609b6edc1d12eedec43400ddd141ceb189eff64854612a"
  }
];
const hash=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');

/** Preserve the close disposition of typed CLOEXEC queues during host exec.
 * Apply last, after the complete owned Linux ABI projection is prepared. */
export function prepareExecQueueRetirement(output){
 const current=sources.map(row=>hash(path.join(output,row.path)));
 if(sources.every((row,index)=>current[index]===row.sha256))return;
 for(const [index,row] of sources.entries())if(current[index]!==row.beforeSha256)throw Error('Linux exec source changed: '+row.path);
 execFileSync('git',['apply','--unsafe-paths',path.join(import.meta.dirname,'adapters/aim-exec-queue.patch')],{cwd:output,stdio:'inherit',env:{...process.env,GIT_CEILING_DIRECTORIES:path.dirname(output)}});
 for(const row of sources)if(hash(path.join(output,row.path))!==row.sha256)throw Error('Linux exec adaptation did not match: '+row.path);
}
