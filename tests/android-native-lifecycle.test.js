const test=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const os=require('node:os');
const path=require('node:path');
const {spawnSync}=require('node:child_process');

test('Bionic guest finalization preserves ownership, concurrent extraction and atfork removal',async t=>{
  if(process.platform!=='darwin'||process.arch!=='arm64')return t.skip('Requires macOS arm64');
  const bionic=process.env.ANDROID_BIONIC_SOURCE,klepton=process.env.ANDROID_KLEPTON_SOURCE;
  if(!bionic||!klepton)return t.skip('Set pinned Bionic/Klepton sources');
  const directory=fs.mkdtempSync(path.join(os.tmpdir(),'android-lifecycle-'));
  t.after(()=>fs.rmSync(directory,{recursive:true,force:true}));
  const {prepareNativeSurfaceSources}=await import('../src/modules/android/native/native-surface-sources.mjs');
  prepareNativeSurfaceSources(klepton,directory);
  const {prepareNativeLifecycle}=await import('../src/modules/android/native/native-lifecycle/build.mjs');
  const result=prepareNativeLifecycle({bionicSource:bionic,klepton,build:directory});
  const native=path.resolve(__dirname,'../src/modules/android/native'),fixture=path.join(__dirname,'fixtures/android-native-lifecycle');
  function run(command,args){const result=spawnSync(command,args,{encoding:'utf8',timeout:30000});assert.equal(result.error,undefined,result.error?.message);assert.equal(result.status,0,result.stdout+result.stderr);return result.stdout;}
  const memory=path.join(directory,'memory.o'),binary=path.join(directory,'probe');
  run('clang',['-std=c11','-O2','-I'+native,'-c',path.join(fixture,'memory.c'),'-o',memory]);
  run('clang++',['-std=c++20','-O2',path.join(fixture,'probe.cpp'),...result.objects,...result.linkFlags,memory,'-o',binary]);
  const log=run(binary,[]);
  assert.match(log,/10000 concurrent finalizers and atfork removal/);
  assert.match(log,/PASS native exit thread\/global finalization and status/);
  assert.doesNotMatch(fs.readFileSync(result.shim,'utf8'),/static int\s+kl_cxa_atexit|static void kl_cxa_finalize/);
});
