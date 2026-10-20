const test=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),os=require('node:os'),path=require('node:path');
const {spawnSync}=require('node:child_process');
const fixture=path.join(__dirname,'fixtures/android-native');
function run(program,args,env=process.env){
 const result=spawnSync(program,args,{env,encoding:'utf8',timeout:30000,maxBuffer:2<<20});
 assert.ifError(result.error);assert.equal(result.signal,null,result.stdout+result.stderr);
 assert.equal(result.status,0,result.stdout+result.stderr);return result.stdout;
}
test('JVM and ART instruction evidence recover complete original canary continuations',async t=>{
 const klepton=process.env.KLEPTON_SOURCE||path.resolve('src/external/klepton');
 if(process.platform!=='darwin'||process.arch!=='arm64'||!fs.existsSync(path.join(klepton,'runtime/kl_x18.c')))
  return t.skip('Requires Apple Silicon and the pinned Klepton source');
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-protected-tail-'));
 t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const {prepareX18Source,prepareX18Core}=await import('../src/modules/android/native/x18-source.mjs');
 for(const [name,prepare,define] of [['jvm',prepareX18Source,'KL_X18_SOURCE'],['art',prepareX18Core,'KL_X18_CORE_SOURCE']]){
  const folder=path.join(root,name);fs.mkdirSync(folder);const source=prepare(klepton,folder),binary=path.join(folder,'proof');
  run('clang',['-O1','-g','-fsanitize=address,undefined','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function',
   '-I'+folder,'-I'+path.join(klepton,'runtime'),'-D'+define+'='+JSON.stringify(source),
   path.join(fixture,'protected-tail-evidence.c'),...(name==='jvm'?[path.join(klepton,'runtime/kl_env.c')]:[]),'-o',binary]);
  assert.match(run(binary,[]),/PASS original canary continuation, opaque prefix, explicit data and function boundaries/);
 }
});
test('native ELF loader executes intact protected body with original Android TP and x18',async t=>{
 const core=process.env.KLEPTON_NATIVE_CORE,clang=process.env.ANDROID_CLANG;
 if(process.platform!=='darwin'||process.arch!=='arm64'||!core||!clang)
  return t.skip('Requires Apple Silicon, selected native core and Android Clang');
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-protected-elf-'));
 t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const identities=path.join(root,'process-ids');fs.mkdirSync(identities,{mode:0o700});
 const env={...process.env,KL_ANDROID_ID_DIRECTORY:identities};
 const guest=path.join(root,'libprotected.so'),binary=path.join(root,'proof');
 run(clang,['-shared','-nostdlib','-Wl,--hash-style=both','-Wl,-z,max-page-size=16384',path.join(fixture,'protected-tail-guest.S'),'-o',guest],env);
 run('clang',['-O2','-Wall','-Wextra','-Werror',path.join(fixture,'protected-tail.c'),'-o',binary],env);
 assert.match(run(binary,[path.resolve(core),guest,env.KL_ANDROID_ID_DIRECTORY],env),/PASS actual ELF continuation executes Android TP\/canary and x18/);
});
