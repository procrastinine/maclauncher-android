const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),os=require('node:os'),{spawnSync}=require('node:child_process');
test('Guest dumpability restricts actual same-principal process memory and survives fork/exec correctly',async t=>{
 if(process.platform!=='darwin'||process.arch!=='arm64')return t.skip('Requires Apple Silicon process authority');
 const bionicSource=process.env.ANDROID_BIONIC_SOURCE;
 if(!bionicSource)return t.skip('Requires pinned original Bionic UAPI constants');
 const temporary=fs.mkdtempSync(path.join(os.tmpdir(),'android-dumpability-'));t.after(()=>fs.rmSync(temporary,{recursive:true,force:true}));
 const native=path.resolve(__dirname,'../src/modules/android/native'),binary=path.join(temporary,'probe');
 const {preparePrctlConstants}=await import('../src/modules/android/native/process-abi/prctl.mjs');
 preparePrctlConstants({bionicSource,output:path.join(temporary,'process-abi')});
 const run=(file,args)=>{const result=spawnSync(file,args,{encoding:'utf8',timeout:25000});assert.ifError(result.error);assert.equal(result.signal,null,result.stdout+result.stderr);assert.equal(result.status,0,result.stdout+result.stderr);return result.stdout;};
 run('clang',['-arch','arm64','-mmacosx-version-min=13.0','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-I'+native,'-I'+temporary,path.join(native,'process-ids.c'),path.join(native,'process-memory.c'),path.join(__dirname,'fixtures/android-native/process-dumpability.c'),'-o',binary]);
 const ids=path.join(temporary,'ids');fs.mkdirSync(ids,{mode:0o700});
 assert.match(run(binary,[ids]),/PASS guest dumpability defaults/);
});
