import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';
const sources=[
  {
    "path": "crates/aim-linux-abi/src/sys/exec.rs",
    "beforeSha256": "29ddd04d663d5559ce19cf400e4018bbc221a1258fee3c553ef764826dc3c991",
    "sha256": "f1d4fb916dfcbd0bf6210ea2db97f03e611d7ab5fa4e8c0da04f337ff5372f1f"
  },
  {
    "path": "crates/aim-linux-abi/src/sys/ptimer.rs",
    "beforeSha256": "33cc2131e868b01b8349d9260ff60504acc06547d0569d5ba68d9ae333d1c9b6",
    "sha256": "90078ff707d3fdb32dcd1657675183031c80d19805c33229176464ee2ef3ec5f"
  },
  {
    "path": "crates/aim-linux-abi/src/sys/fdtab.rs",
    "beforeSha256": "86b9374b040821fd524b745eb3c8ae46654116b0edf61b56e741a1737fb5a53b",
    "sha256": "494b04026ab2bd9cb6e350fb060050cf82599d4fcd7ab8b727f153f0eaee2d9d"
  },
  {
    "path": "crates/aim-linux-abi/src/sys/fs.rs",
    "beforeSha256": "49d46fcdad9b6ffef80bc4bfdff7f999262bb14ba510336fe34c50c73461e651",
    "sha256": "e829855f46b294ee9964b8e253983a755a119c5f52b740ab1335f201165c046b"
  }
];
const hash=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');

/** Preserve queues through in-place exec when POSIX timers are present, and
 * map descriptor flags at the Darwin boundary. Apply after queue retirement. */
export function prepareTimerExec(output){
 const current=sources.map(row=>hash(path.join(output,row.path)));
 if(sources.every((row,index)=>current[index]===row.sha256))return;
 for(const [index,row] of sources.entries())if(current[index]!==row.beforeSha256)throw Error('Linux timer exec source changed: '+row.path);
 // Prepared trees can share immutable original files. Detach each affected
 // inode before the patch so the original source remains read-only.
 for(const row of sources){const file=path.join(output,row.path),stat=fs.lstatSync(file);if(stat.isSymbolicLink()||stat.nlink>1){const temporary=file+'.exec-timers-'+process.pid;try{fs.copyFileSync(file,temporary,fs.constants.COPYFILE_EXCL);fs.chmodSync(temporary,fs.statSync(file).mode&0o777);fs.renameSync(temporary,file);}finally{fs.rmSync(temporary,{force:true});}}}
 execFileSync('git',['apply','--unsafe-paths',path.join(import.meta.dirname,'adapters/aim-exec-timers.patch')],{cwd:output,stdio:'inherit',env:{...process.env,GIT_CEILING_DIRECTORIES:path.dirname(output)}});
 for(const row of sources)if(hash(path.join(output,row.path))!==row.sha256)throw Error('Linux timer exec adaptation did not match: '+row.path);
}
