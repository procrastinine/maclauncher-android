import test from 'node:test';import assert from 'node:assert/strict';import fs from 'node:fs';import path from 'node:path';import os from 'node:os';import {spawnSync} from 'node:child_process';
import {prepareMemoryBudget} from '../src/modules/android/art/memory-budget-source.mjs';
const fixture=path.join(import.meta.dirname,'fixtures/android-art-memory-budget');
const run=(command,args,options={})=>{const result=spawnSync(command,args,{encoding:'utf8',timeout:60000,maxBuffer:4<<20,...options});assert.ifError(result.error);assert.equal(result.signal,null,result.stdout+result.stderr);assert.equal(result.status,0,result.stdout+result.stderr);return result.stdout;};

test('ART memory overlay retains original ABI and reuses one existing accounting owner',t=>{
 const source=process.env.ANDROID_AIM_SOURCE;if(!source)return t.skip('Requires pinned AIM source input');
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-memory-source-'));t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 for(const file of ['crates/aim-host-memory/src/lib.rs','crates/aim-linux-abi/src/sys/procfs.rs','crates/aim-services/src/app_host/job_scheduler.rs']){fs.mkdirSync(path.dirname(path.join(root,file)),{recursive:true});fs.copyFileSync(path.join(source,file),path.join(root,file));}
 prepareMemoryBudget(root);
 assert.match(fs.readFileSync(path.join(root,'crates/aim-host-memory/src/lib.rs'),'utf8'),/guest_budget::snapshot\(total\(\)\)/);
 assert.match(fs.readFileSync(path.join(root,'crates/aim-linux-abi/src/sys/procfs.rs'),'utf8'),/let memory=aim_host_memory::guest_memory\(\)/);
 assert.match(fs.readFileSync(path.join(root,'crates/aim-services/src/app_host/job_scheduler.rs'),'utf8'),/let bytes = aim_host_memory::total\(\)/);
});

test('ART memory policy matches native MiB parsing and tracks actual private pages',{timeout:90000},t=>{
 if(process.platform!=='darwin'||process.arch!=='arm64')return t.skip('Requires Apple Silicon macOS');
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-memory-owner-'));t.after(()=>fs.rmSync(root,{recursive:true,force:true}));fs.mkdirSync(path.join(root,'src'));
 const helper=path.resolve(import.meta.dirname,'../src/modules/android/art/native/memory-budget.rs');
 fs.writeFileSync(path.join(root,'src/main.rs'),'#[path='+JSON.stringify(helper)+'] mod memory_budget;\n'+fs.readFileSync(path.join(fixture,'owner.rs'),'utf8'));
 fs.writeFileSync(path.join(root,'Cargo.toml'),'[package]\nname="android-memory-owner-probe"\nversion="0.0.0"\nedition="2024"\n[dependencies]\nlibc="0.2"\n');
 run(process.env.CARGO||'cargo',['build','--offline','--release','--manifest-path',path.join(root,'Cargo.toml')]);
 const binary=path.join(root,'target/release/android-memory-owner-probe');
 for(const [configured,mib]of [[undefined,6144],['384',384],['0x180',384],['0600',384],['invalid',0],['0',0],['-1',-1],['2147483647',2147483647]]){
  const env={...process.env};if(configured===undefined)delete env.KL_MEM_TOTAL_MB;else env.KL_MEM_TOTAL_MB=configured;
  const requested=BigInt.asUintN(64,BigInt(mib)<<20n);const output=run(binary,[String(requested)],{env});assert.match(output,/PASS actual native memory owner/);t.diagnostic(output.trim());
 }
});

test('original Bionic memory APIs report the same guest budget',{timeout:90000},t=>{
 const {ANDROID_ART_MEMORY_RUNNER:runner,ANDROID_ART_MEMORY_MAP:map,ANDROID_ART_MEMORY_DIRECTORY:directory,ANDROID_CLANG:clang}=process.env;
 if(![runner,map,directory,clang].every(Boolean))return t.skip('Requires a prepared ART probe map, directory, runner and Android Clang');
 fs.mkdirSync(directory,{recursive:true});const binary=path.join(directory,'probe');
 run(clang,['-O2','-Wall','-Wextra','-Werror',path.join(fixture,'main.c'),'-o',binary]);
 const env=JSON.parse(process.env.ANDROID_ART_MEMORY_ENV||'{}');
 const physical=BigInt(run('/usr/sbin/sysctl',['-n','hw.memsize']).trim());
 for(const mib of [384,768,6144]){
  const expected=BigInt(mib)*1024n*1024n<physical?BigInt(mib)*1024n*1024n:physical;
  const output=run(runner,['--inherit-env','--runtime',path.join(directory,'kernel-'+mib),'--no-cache','--path-map',map,'/data/local/tmp/memory-probe/probe',String(expected)],{env:{...env,KL_MEM_TOTAL_MB:String(mib)}});
  assert.match(output,/PASS original Bionic sysinfo, sysconf physical\/available pages and proc/);t.diagnostic(output.trim());
 }
});
