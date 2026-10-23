import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';
const sources=[
  {
    "path": "crates/aim-binder-driver/src/state.rs",
    "beforeSha256": "605f5c4f95e9c4da1ecee285fa039ee3f4eaf12542e8bb6d00ef0f68fa1ef977",
    "sha256": "f07941f53aadf2fcee20b4430f11ed7b8c9632bccebee8720d549a71468f15cb"
  },
  {
    "path": "crates/aim-binder-driver/src/driver.rs",
    "beforeSha256": "3f0e263b7dfa5ee3c4dd7cdcbdabab55ce468cd13f5fe30ea4f2625a4ee0ee6a",
    "sha256": "b521e53fb7e313187c2d70cb8790f6b861966d7ff1d968ab5b5163dd868c3659"
  },
  {
    "path": "crates/aim-binder-driver/src/transaction.rs",
    "beforeSha256": "e711db6b936f2b1e56129cf78baeb7cd9a3b8d78aad52a7df56a90dda8d7ddb6",
    "sha256": "8bb99cb268998ab7de218512eb0b087885f2d7956b363c99b2e0f7d71ff1f38a"
  },
  {
    "path": "crates/aim-binder-host/src/wire.rs",
    "beforeSha256": "47dbef54b04989e90893ae0839d8527e6c8093fd436a66ba3661f5930d5ddc8c",
    "sha256": "ed3e62b9aaab47abe297a6dda3b7fd5751a8d758d086fc2d548aa0f9a3e52803"
  },
  {
    "path": "crates/aim-binder-host/src/client.rs",
    "beforeSha256": "f25392093a9c29082df446a307e30a8a80424f660cc1e74d3c8bca0f95dbd9ab",
    "sha256": "36262324fbf8945e2fef670644a754db1ba487cdc06be00df8fee7d31d713e2d"
  },
  {
    "path": "crates/aim-binder-host/src/server.rs",
    "beforeSha256": "70f140440c2b0ec454f12e90a2eefc37093a799d3404093afe855cd734ce15b7",
    "sha256": "341c0892fbdd568e788a4e6907fa279d525a3774d6ae7fff0791edb56d0bba4b"
  },
  {
    "path": "crates/aim-binder-host/src/bin/aim-binderd.rs",
    "beforeSha256": "856514614f0521ee8565034aa2ea056ec0b5bd5577205942a7c7bbbcae357079",
    "sha256": "5f15888d3266521b2102bd706a3ab8914a1c12cdac1353d0440706f1b3908d1f"
  },
  {
    "path": "crates/aim-linux-abi/src/sys/binder.rs",
    "beforeSha256": "901f8446d0fe8144bf2ec775783516e5c7ddd2dff6d09d338b34a0c37f1a7a47",
    "sha256": "25b5e05411aa24c97d41d39c4016b2a441d651157e6409c16631a54825b754a3"
  },
  {
    "path": "crates/aim-linux-abi/src/sys/cred.rs",
    "beforeSha256": "8d1d5c24030cd238ef1e1099864df4c9d09d8618e7ea28fdd5c54308536bcaee",
    "sha256": "3f9055e89118f0aff6cba1a3554a0942a7ec54ff530566fd2bcbac794933974c"
  }
];
const hash=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
/** Authenticated credential transitions refresh Binder sender UID while the
 * original open security credentials and forwarded identities stay distinct. */
export function prepareBinderSenderIdentity(output){
 const current=sources.map(row=>hash(path.join(output,row.path)));
 if(sources.every((row,index)=>current[index]===row.sha256))return;
 for(const [index,row] of sources.entries())if(current[index]!==row.beforeSha256)throw Error('Binder identity source changed: '+row.path);
 for(const row of sources){const file=path.join(output,row.path),stat=fs.lstatSync(file);if(stat.isSymbolicLink()||stat.nlink>1){const temporary=file+'.binder-sender-'+process.pid;try{fs.copyFileSync(file,temporary,fs.constants.COPYFILE_EXCL);fs.chmodSync(temporary,fs.statSync(file).mode&0o777);fs.renameSync(temporary,file);}finally{fs.rmSync(temporary,{force:true});}}}
 execFileSync('git',['apply','--unsafe-paths',path.join(import.meta.dirname,'adapters/aim-binder-sender.patch')],{cwd:output,stdio:'inherit',env:{...process.env,GIT_CEILING_DIRECTORIES:path.dirname(output)}});
 for(const row of sources)if(hash(path.join(output,row.path))!==row.sha256)throw Error('Binder identity adaptation did not match: '+row.path);
}
