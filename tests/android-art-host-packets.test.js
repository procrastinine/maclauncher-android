const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),os=require('node:os'),{spawnSync}=require('node:child_process');
test('Typed C packet ABI preserves access, values, errors and installer identity',t=>{
 if(process.platform!=='darwin')return t.skip('Requires the native macOS C toolchain');
 const temporary=fs.mkdtempSync(path.join(os.tmpdir(),'android-host-packets-'));t.after(()=>fs.rmSync(temporary,{recursive:true,force:true}));
 const native=path.resolve(__dirname,'../src/modules/android/art/native'),binary=path.join(temporary,'packets');
 const run=(file,args)=>{const r=spawnSync(file,args,{encoding:'utf8',timeout:30000});assert.ifError(r.error);assert.equal(r.signal,null,r.stderr);assert.equal(r.status,0,r.stdout+r.stderr);};
 run('clang',['-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+native,path.join(native,'host-packets.c'),path.join(__dirname,'fixtures/android-native/art-host-packets.c'),'-o',binary]);
 run(binary,[]);
});
