const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),os=require('node:os'),path=require('node:path');
const {spawnSync}=require('node:child_process');
test('Large decrypted executable publication uses fresh evidence and an atomic reachable-pool plan',{timeout:60000},t=>{
 const core=process.env.KLEPTON_NATIVE_CORE,clang=process.env.ANDROID_CLANG;
 if(process.platform!=='darwin'||process.arch!=='arm64'||!core||!clang)return t.skip('Requires Apple Silicon, the native core and Android Clang');
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-code-publication-'));t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const ids=path.join(root,'ids');fs.mkdirSync(ids,{mode:0o700});const env={...process.env,KL_ANDROID_ID_DIRECTORY:ids};
 const fixture=path.resolve(__dirname,'fixtures/android-native'),native=path.resolve(__dirname,'../src/modules/android/native');
 const run=(program,args)=>{const r=spawnSync(program,args,{env,encoding:'utf8',timeout:45000,maxBuffer:2<<20});assert.ifError(r.error);assert.equal(r.signal,null,r.stdout+r.stderr);assert.equal(r.status,0,r.stdout+r.stderr);return r.stdout+r.stderr;};
 const guest=path.join(root,'libpublication.so');run(clang,['-nostdlib','-shared','-Wl,--hash-style=both','-Wl,-z,max-page-size=16384','-Wl,-T,'+path.join(fixture,'inline-range.ld'),path.join(fixture,'code-publication-guest.S'),'-o',guest]);
 assert(fs.statSync(guest).size<128*1024);
 const probe=path.join(root,'probe');run('clang',['-O2','-Wall','-Wextra','-Werror','-I'+native,path.join(fixture,'code-publication.c'),path.join(fixture,'code-publication-state.S'),path.join(fixture,'inline-syscall.S'),'-o',probe]);
 const result=run(probe,[path.resolve(core),guest,...(process.env.KLEPTON_PUBLICATION_BASELINE?['baseline']:[])]);
 assert.match(result,/PASS large decrypted publication retains original TP, SVC machine state, x18, source data, gap leases and repeated publication/);
 assert.match(result,/PASS whole-range refusal preserves original instructions, gap reservations and all requested mapping permissions/);
});
