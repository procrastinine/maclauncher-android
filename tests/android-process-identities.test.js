const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),os=require('node:os'),path=require('node:path'),{spawnSync}=require('node:child_process');
test('Both execution bindings preserve native thread retirement, trace-held IDs and retained child waits',t=>{
 if(process.platform!=='darwin')return t.skip('Requires the macOS physical thread owner');
 const temporary=fs.mkdtempSync(path.join(os.tmpdir(),'android-identities-'));t.after(()=>fs.rmSync(temporary,{recursive:true,force:true}));
 const directory=path.join(temporary,'ids');fs.mkdirSync(directory,{mode:0o700});
 const native=path.resolve(__dirname,'../src/modules/android/native'),binary=path.join(temporary,'identities');
 const run=(command,args)=>{const result=spawnSync(command,args,{encoding:'utf8',timeout:20000});assert.equal(result.error,undefined);assert.equal(result.status,0,result.stdout+result.stderr);return result.stdout;};
 run('clang',['-std=gnu11','-O2','-Wall','-Wextra','-Werror','-I'+native,native+'/process-ids.c',native+'/thread-identity.c',path.join(__dirname,'fixtures/android-native/process-identities.c'),'-o',binary]);
 const output=run(binary,[directory]);
 assert.match(output,/PASS shared compact process\/thread leases/);
 assert.match(output,/PASS trace holds retain only IDs through physical retirement, counted wait release, exact TID reuse, early wait TLS identity and fork isolation/);
});
