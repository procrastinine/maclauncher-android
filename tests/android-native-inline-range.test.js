const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),os=require('node:os'),path=require('node:path');
const {spawnSync}=require('node:child_process');
test('Large original ELF mappings route TP, syscall and original x18 bodies through reachable owned pools',t=>{
 const core=process.env.KLEPTON_NATIVE_CORE,clang=process.env.ANDROID_CLANG;
 if(process.platform!=='darwin'||process.arch!=='arm64'||!core||!clang)return t.skip('Requires Apple Silicon, the changed native core and Android Clang');
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-inline-range-'));t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const ids=path.join(root,'ids');fs.mkdirSync(ids,{mode:0o700});const env={...process.env,KL_ANDROID_ID_DIRECTORY:ids};
 const fixture=path.resolve(__dirname,'fixtures/android-native'),native=path.resolve(__dirname,'../src/modules/android/native');
 const run=(file,args)=>{const r=spawnSync(file,args,{env,encoding:'utf8',timeout:30000,maxBuffer:2<<20});assert.ifError(r.error);assert.equal(r.signal,null,r.stdout+r.stderr);assert.equal(r.status,0,r.stdout+r.stderr);return r.stdout+r.stderr;};
 const guest=path.join(root,'librange.so');run(clang,['-nostdlib','-shared','-Wl,--hash-style=both','-Wl,-z,max-page-size=16384','-Wl,-T,'+path.join(fixture,'inline-range.ld'),path.join(fixture,'inline-range-guest.S'),'-o',guest]);
 const refused=path.join(root,'librefused.so');run(clang,['-nostdlib','-shared','-Wl,--hash-style=both','-Wl,-z,max-page-size=16384',path.join(fixture,'inline-refused-guest.S'),'-o',refused]);
 assert(fs.statSync(guest).size<128*1024,'large virtual ranges stay sparse in the original ELF');
 const probe=path.join(root,'probe');run('clang',['-O2','-Wall','-Wextra','-Werror','-I'+native,path.join(fixture,'inline-range.c'),'-o',probe]);
 const result=run(probe,[path.resolve(core),guest,refused]);
 assert.match(result,/PASS sparse large ELF executes original TP\/SVC\/x18 through outer and interior pools/);
 assert.match(result,/PASS rejected original TP candidate retains its precise registry reason and bounded guest dlerror/);
 assert.match(result,/guest dlopen\([^\n]+FAILED: Android native library has 1 unsupported TLS sites/);
});
