import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';
const sources=[
  {
    "path": "crates/aim-linux-abi/src/sys/exec.rs",
    "beforeSha256": "d7bffeeecf5dd2f54cdf22e5d3f1793ae7283e6368124417ef1d6a8397e607b9",
    "sha256": "d54a37aae6fc6afce1511c6c861c5f2e870146bc9feb0d7f1825345ee8f66ac7"
  },
  {
    "path": "crates/aim-linux-abi/src/sys/ptimer.rs",
    "beforeSha256": "13974a5b1b6b3eb3e5b9fc31412ca9ea0db3c6e3b2542d1821b658a83d7ca754",
    "sha256": "69eeec41353270a6f413acf5d3364921dc2a7d21ae519e3a01c76168f7edd7a9"
  },
  {
    "path": "crates/aim-linux-abi/src/sys/fork/state.rs",
    "beforeSha256": "70c7b7d74351ec46f145ad181f31552fcd614366b76192ce5b91e0f24ac42195",
    "sha256": "9c0406a0d16cf40f7171dc2be92920000210771623bd4e51c3f9cff07a7fc2cc"
  }
];
const matches=(expected,actual)=>[expected].flat().includes(actual);
const hash=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');

/** Keep timer deadlines on their selected clock, including host sleep and
 * wall-clock changes. Preserve timer retirement through in-place exec and
 * descriptor flags at the Darwin boundary. Apply after queue retirement. */
export function prepareTimerExec(output){
 const current=sources.map(row=>hash(path.join(output,row.path)));
 if(sources.every((row,index)=>matches(row.sha256,current[index])))return;
 for(const [index,row] of sources.entries())if(!matches(row.beforeSha256,current[index]))throw Error('Linux timer exec source changed: '+row.path);
 // Prepared trees can share immutable original files. Detach each affected
 // inode before the patch so the original source remains read-only.
 for(const row of sources){const file=path.join(output,row.path),stat=fs.lstatSync(file);if(stat.isSymbolicLink()||stat.nlink>1){const temporary=file+'.exec-timers-'+process.pid;try{fs.copyFileSync(file,temporary,fs.constants.COPYFILE_EXCL);fs.chmodSync(temporary,fs.statSync(file).mode&0o777);fs.renameSync(temporary,file);}finally{fs.rmSync(temporary,{force:true});}}}
 execFileSync('git',['apply','--unsafe-paths',path.join(import.meta.dirname,'adapters/aim-exec-timers.patch')],{cwd:output,stdio:'inherit',env:{...process.env,GIT_CEILING_DIRECTORIES:path.dirname(output)}});
 for(const row of sources)if(!matches(row.sha256,hash(path.join(output,row.path))))throw Error('Linux timer exec adaptation did not match: '+row.path);
}
