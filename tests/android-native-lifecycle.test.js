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


test('Bionic semaphore slots retain more than 1024 live owners and recycle them',async t=>{
  if(process.platform!=='darwin')return t.skip('Requires macOS dispatch semaphores');
  const klepton=process.env.ANDROID_KLEPTON_SOURCE;
  if(!klepton)return t.skip('Set the pinned Klepton source');
  const directory=fs.mkdtempSync(path.join(os.tmpdir(),'android-semaphores-'));
  t.after(()=>fs.rmSync(directory,{recursive:true,force:true}));
  const {prepareNativeSurfaceSources}=await import('../src/modules/android/native/native-surface-sources.mjs');
  prepareNativeSurfaceSources(klepton,directory);
  const source=fs.readFileSync(path.join(directory,'pthread-source.c'),'utf8');
  const start=source.indexOf('// ---------- semaphores ----------'),end=source.indexOf('// sem_open/sem_close:',start);
  assert.ok(start>=0&&end>start);
  const fixture=path.join(directory,'semaphores.c'),binary=path.join(directory,'semaphores');
  fs.writeFileSync(fixture,`#include <assert.h>
#include <errno.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#define kl_env_on(...) 0
#define sem_wait_enter(...)
#define sem_wait_leave(...)
${source.slice(start,end)}
int main(void){
 int slots[4096];
 for(int i=0;i<4096;i++){assert(klb_sem_init(&slots[i],0,0)==0);for(int j=0;j<i;j++)assert(slots[j]!=slots[i]);}
 for(int i=0;i<4096;i++){assert(klb_sem_post(&slots[i])==0);assert(klb_sem_trywait(&slots[i])==0);assert(klb_sem_trywait(&slots[i])==-1&&errno==EAGAIN);}
 for(int i=0;i<4096;i++)assert(klb_sem_destroy(&slots[i])==0&&slots[i]==0);
 int next=atomic_load(&g_nsems);
 for(int i=0;i<4096;i++)assert(klb_sem_init(&slots[i],0,0)==0);
 assert(atomic_load(&g_nsems)==next);
 for(int i=0;i<4096;i++)assert(klb_sem_destroy(&slots[i])==0);
 puts("PASS 4096 simultaneous Bionic semaphores, counts and slot reuse");
}
`);
  for(const [command,args]of [['clang',['-std=c11','-O2','-fblocks',fixture,'-o',binary]],[binary,[]]]){
    const result=spawnSync(command,args,{encoding:'utf8',timeout:30000});
    assert.equal(result.error,undefined,result.error?.message);assert.equal(result.status,0,result.stdout+result.stderr);
    if(command===binary)assert.match(result.stdout,/PASS 4096 simultaneous Bionic semaphores/);
  }
});
