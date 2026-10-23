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
    "sha256": "d2aea0ae9775af4d9817a9c2e6ada5a70a633fb3b68e10bdc7464831fdbc27b3"
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
  },
  {
    "path": "crates/aim-linux-abi/src/sys/clock.rs",
    "beforeSha256": "4156d2860e4e565124ba57134d0ebc850c72057286e67b1ba0c2415084318f93",
    "sha256": "f0675900ca845950fa9dc8dd7299fc778429fe486e77d45a8b1abdcb370c174d"
  },
  {
    "path": "crates/aim-linux-abi/src/sys/event.rs",
    "beforeSha256": "82efb5c53070228d48248e52dc9d5e6c08dafa967d096f4877df51ea9c8192b9",
    "sha256": "ab11b0cb06f03495c9d43e4d74683ecd12355cf9436dc7f685c9349ca5a8f17a"
  },
  {
    "path": "crates/aim-linux-abi/src/sys/futex.rs",
    "beforeSha256": "a9fee8a5c30ebc79de91c1aed4c4a2468dc8fa804abf3e6969268b5484fefb98",
    "sha256": "e31eecafd15afa83667354d1d4fa0cf8d27c2c5a606de3d17c702c6e0e595d67"
  },
  {
    "path": "crates/aim-linux-abi/src/sys/park.rs",
    "beforeSha256": "7cb3559258e6997b2e8d3a1cf4f0a605f99792bcf51960b0429001dbe1e7c22a",
    "sha256": "18005d190cf6ff62b200e1b6ec3faf3f191749e481373cfce1ecc9daa2217b59"
  },
  {
    "path": "crates/aim-linux-abi/src/sys/signal.rs",
    "beforeSha256": [
      "3d2fa6e67c1befebc1719f9c7909d44c3f7ad83aab83a3e7200ac1fdf0d3e3ad",
      "bfa63fb4e3ca5491b12575a5ed4cca49cb03b219cd6c6a8454360bbba50cb59a"
    ],
    "sha256": [
      "7e63b20cc1a9ca43d644623ab196564a4e2304e46c29d2aa20a37c2a2d9f1826",
      "5bca371716f3c8ff64fb59aa6f9648a8dafb8f815aef895d2eeb76222374aa41"
    ]
  },
  {
    "path": "crates/aim-linux-abi/src/sys/fork/state.rs",
    "beforeSha256": [
      "9a553cfa739b35a1d1551df58650295c8707d3a4b7d443c99fdc87ac5873e5e9",
      "d8c115495e98e652229ea9fa8f2240aea1bb3a748575a71d54bf2d429d9d79ce"
    ],
    "sha256": [
      "b6b1bb0903c6719de414f7dad1d447c34ecb16b3fa3c525a9c6f709c48ad677b",
      "c7ade1629541f187a336fb504edfb0f966ba9ec8dec7c83ec6eb5bfbae1d4718"
    ]
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
