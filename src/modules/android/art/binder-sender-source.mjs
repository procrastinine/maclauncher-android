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
    "beforeSha256": "9c8673d8f1b7b9e3f4bbb49bdb3e7b14f9f20ff3803ad055beb45a08cd727502",
    "sha256": "23983ca8baa57b81262703b90b6d56ecdd3f96c4fc309dbeab3a8929c4f97e79"
  },
  {
    "path": "crates/aim-binder-driver/src/transaction.rs",
    "beforeSha256": "d356c3dd04bea784022d7e2ac2d9f9ba19aa666b1a7a2abf6197d78a046fab76",
    "sha256": "e7e5df7752bb8890838dfbd409bcf772338b10c05d8541c516d1bcbdbf972eec"
  },
  {
    "path": "crates/aim-binder-host/src/wire.rs",
    "beforeSha256": "0a9fcb263215ed9a57ff27c0f3d07dcd687cdec0f8b4e328accab279bbe00d4c",
    "sha256": "bd273605abd6d02ce7664cd223755ca86b0e223cf2a127b663e865dc147f56cc"
  },
  {
    "path": "crates/aim-binder-host/src/client.rs",
    "beforeSha256": "988a2293485483502566a8f44bab72fa454aa92b9f2f9d46ea2ec3dc97263950",
    "sha256": "76d86aef5013a4af1600b5acf809211d2414c1ff936a5507afd6fd6ad121fcad"
  },
  {
    "path": "crates/aim-binder-host/src/server.rs",
    "beforeSha256": "0ca34d94191a6a899aea7f014153ce07ebe6d145bfbc29b816634c959700c3fd",
    "sha256": "c1874fdb135b96cb1af268bdeb94e8ec0404ca89f7ea10420a0725aee8295a72"
  },
  {
    "path": "crates/aim-binder-host/src/bin/aim-binderd.rs",
    "beforeSha256": "856514614f0521ee8565034aa2ea056ec0b5bd5577205942a7c7bbbcae357079",
    "sha256": "5f15888d3266521b2102bd706a3ab8914a1c12cdac1353d0440706f1b3908d1f"
  },
  {
    "path": "crates/aim-linux-abi/src/sys/binder.rs",
    "beforeSha256": "7f0b9f3c8f4a733dd8c36afee41fdc67e46bd5c77792f1aa0911e0efc9191ad7",
    "sha256": "6762819f19b0979b872d645565d286396b53ef4d19ad9f45c458d9e5c7ebb2b6"
  },
  {
    "path": "crates/aim-linux-abi/src/sys/cred.rs",
    "beforeSha256": "e39fb583284082e2b1b0e7e491c5df9a57687cacfb6043426256a6b41f3a61a7",
    "sha256": "8433af4d309b25fe21756b71a304208c4c3b5c4ebb8d426479ec142edff38db0"
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
