import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';
import {buildPrecise} from '../native/arm64/precise-build.mjs';
import {nativePathFlags,nativeRustPathEnvironment,publishNativeBinary} from '../native/upstream/native-paths.mjs';

/** Keep the pinned supplier's license exceptions and verbatim third-party
 * notices with its native products, alongside the supplied CPU notices. */
export function stageArtRunnerNotices({aim,cpuNotices,output}){
 aim=path.resolve(aim);cpuNotices=path.resolve(cpuNotices);output=path.resolve(output);
 const required=['LICENSE','NOTICE','LICENSING.md','THIRD_PARTY_NOTICES.md','licensing/OPENJDK.md','licensing/rust-dependencies.txt','licensing/upstream-sources.json'];
 for(const name of required)if(!fs.statSync(path.join(aim,name),{throwIfNoEntry:false})?.isFile())throw Error('Missing AIM license input: '+name);
 if(!fs.statSync(path.join(aim,'licensing/third-party'),{throwIfNoEntry:false})?.isDirectory())throw Error('Missing AIM third-party license directory.');
 const catalog=JSON.parse(fs.readFileSync(path.join(aim,'licensing/upstream-sources.json'),'utf8'));
 if(!Array.isArray(catalog.files)||!catalog.files.length)throw Error('Missing AIM third-party license records.');
 for(const row of catalog.files){
  if(typeof row.file!=='string'||!row.file.startsWith('licensing/third-party/')||row.file.split(/[\\/]/).some(part=>part==='..')||!/^[a-f0-9]{64}$/.test(row.sha256))throw Error('Invalid AIM third-party license record.');
  const file=path.join(aim,row.file);if(!fs.statSync(file,{throwIfNoEntry:false})?.isFile())throw Error('Missing AIM license input: '+row.file);
  if(crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex')!==row.sha256)throw Error('AIM license differs from its source record: '+row.file);
 }
 if(!fs.statSync(cpuNotices,{throwIfNoEntry:false})?.isDirectory())throw Error('Missing ARM64 CPU notices.');
 if([aim,cpuNotices].some(input=>output===input||input.startsWith(output+path.sep)||output.startsWith(input+path.sep)))throw Error('ART notice output must be separate from its source inputs.');
 fs.mkdirSync(path.dirname(output),{recursive:true});
 const temporary=fs.mkdtempSync(path.join(path.dirname(output),'.runner-notices-'));
 try{
  fs.cpSync(cpuNotices,temporary,{recursive:true});
  const original=path.join(temporary,'aim');fs.mkdirSync(original,{recursive:true});
  for(const name of ['LICENSE','NOTICE','LICENSING.md','THIRD_PARTY_NOTICES.md','licensing'])fs.cpSync(path.join(aim,name),path.join(original,name),{recursive:true});
  fs.rmSync(output,{recursive:true,force:true});fs.renameSync(temporary,output);
  return output;
 }finally{fs.rmSync(temporary,{recursive:true,force:true});}
}

/** Build a supplied, prepared AIM tree using its original Cargo workspace.
 * A buildPrecise result can reuse existing static CPU archives. Only the runner
 * and its descriptor holders share the upstream notices; compiler products stay in work.
 */
export function buildArtRunner({aim,output,unicorn,precise,work,target,cargo='cargo',env=process.env,jobs=2,pathRoots=[]}){
 if(!aim||!output)throw Error('Expected prepared AIM source and runner output paths.');
 if(!precise&&!unicorn)throw Error('Expected pinned Unicorn source or prepared CPU archives.');
 aim=path.resolve(aim);output=path.resolve(output);work=path.resolve(work||path.join(output,'build'));
 target=path.resolve(target||path.join(work,'cargo'));
 const cpu=precise||buildPrecise({source:unicorn,output:path.join(work,'precise'),jobs});
 if(!Array.isArray(cpu.archives)||!cpu.archives.length||!cpu.notices)throw Error('Expected static ARM64 CPU archives and upstream notices.');
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
 const runnerNotices=stageArtRunnerNotices({aim,cpuNotices:cpu.notices,output:path.join(output,'notices/runner')});
 return {linuxRun,binder,lockHolder,ptyHolder,binderSenderIdentityVersion:1,runnerNotices};
}
