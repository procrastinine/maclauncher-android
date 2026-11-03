import {optionalFiles} from '../native/upstream/source-files.mjs';
import {addLicense,includeLicenses} from '../native/upstream/licenses.mjs';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';
import {buildPrecise} from '../native/arm64/precise-build.mjs';
import {nativePathFlags,nativeRustPathEnvironment,publishNativeBinary} from '../native/upstream/native-paths.mjs';

/** Include the supplier's original licence texts with the runner. */
export function stageArtRunnerLicenses({aim,cpuLicenses,output}){
 includeLicenses(output,cpuLicenses);
 for(const name of ['LICENSE','NOTICE','LICENSING.md','THIRD_PARTY_NOTICES.md','licensing/OPENJDK.md','licensing/rust-dependencies.txt'])addLicense(output,'AIM',path.join(aim,name));
 for(const file of optionalFiles(path.join(aim,'licensing/third-party')))addLicense(output,'AIM/'+path.basename(file),file);
 return path.join(output,'licenses');
}

/** Build a supplied, prepared AIM tree using its original Cargo workspace.
 * A buildPrecise result can reuse existing static CPU archives. Only the runner
 * and its descriptor holders share the upstream licenses; compiler products stay in work.
 */
export function buildArtRunner({aim,output,unicorn,precise,work,target,cargo='cargo',env=process.env,jobs=2,pathRoots=[]}){
 if(!aim||!output)throw Error('Expected prepared AIM source and runner output paths.');
 if(!precise&&!unicorn)throw Error('Expected pinned Unicorn source or prepared CPU archives.');
 aim=path.resolve(aim);output=path.resolve(output);work=path.resolve(work||path.join(output,'build'));
 target=path.resolve(target||path.join(work,'cargo'));
 const cpu=precise||buildPrecise({source:unicorn,output:path.join(work,'precise'),jobs});
 if(!Array.isArray(cpu.archives)||!cpu.archives.length)throw Error('Expected static ARM64 CPU archives.');
 const archives=cpu.archives.map(file=>path.resolve(file));
 for(const file of archives)if(!file.endsWith('.a')||!fs.statSync(file).isFile())throw Error('Expected an ARM64 CPU static archive.');
 const linked=[...archives];
 // Foreign-native writer continuations are a separate source adaptation.
 // Mutable guest execution needs only its supplied precise CPU archives.
 if(fs.existsSync(path.join(aim,'crates/aim-linux-abi/src/sys/native-writer.rs'))){
 const native=path.resolve(import.meta.dirname,'../native/arm64'),objects=path.join(work,'native');
 fs.mkdirSync(objects,{recursive:true});
 const object=path.join(objects,'native-writer.o'),writer=path.join(objects,'libkl_native_writer.a');
 execFileSync('clang++',['-std=c++20','-O2','-g0','-fPIC','-arch','arm64','-mmacosx-version-min=13.0',
  ...nativePathFlags({source:native,build:objects,roots:pathRoots}),'-c',path.join(native,'native-writer.cpp'),'-o',object],{stdio:'inherit'});
 execFileSync('ar',['crs',writer,object],{stdio:'inherit'});
 linked.push(writer);
 }
 const roots=[...(env.CARGO_HOME?[[env.CARGO_HOME,'/src/cargo']]:[]),...pathRoots];
 execFileSync(cargo,['build','--offline','--locked','--release','-j',String(jobs),
  '--manifest-path',path.join(aim,'Cargo.toml'),'--target-dir',target,'-p','aim-linux-abi','--bin','linux-run','-p','aim-binder-host','--bin','aim-binderd','-p','aim-storage','--bin','aim-lock-holder','--bin','aim-pty-holder'],
 {cwd:aim,env:{...env,CARGO_NET_OFFLINE:'true',MACOSX_DEPLOYMENT_TARGET:'13.0',
  MACLAUNCHER_PRECISE_ARCHIVES:linked.join(path.delimiter),
  ...nativeRustPathEnvironment({source:aim,build:target,roots},env)},stdio:'inherit'});
 const linuxRun=path.join(output,'host/linux-run');
 publishNativeBinary(path.join(target,'release/linux-run'),linuxRun);
 const binder=path.join(output,'host/aim-binderd');
 publishNativeBinary(path.join(target,'release/aim-binderd'),binder);
 const lockHolder=path.join(output,'host/aim-lock-holder'),ptyHolder=path.join(output,'host/aim-pty-holder');
 publishNativeBinary(path.join(target,'release/aim-lock-holder'),lockHolder);
 publishNativeBinary(path.join(target,'release/aim-pty-holder'),ptyHolder);
 const runnerLicenses=stageArtRunnerLicenses({aim,cpuLicenses:cpu.licenses,output});
 return {linuxRun,binder,lockHolder,ptyHolder,binderSenderIdentityVersion:1,runnerLicenses};
}
