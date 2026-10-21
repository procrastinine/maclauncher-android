const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),os=require('node:os'),path=require('node:path');
const{spawnSync}=require('node:child_process');
test('original Android ELF POSIX advice and sendfile imports preserve memory, offsets and partial I/O',{timeout:30000},t=>{
 const core=process.env.KLEPTON_NATIVE_CORE,clang=process.env.ANDROID_CLANG;
 if(process.platform!=='darwin'||process.arch!=='arm64'||!core||!clang)return t.skip('Requires Apple Silicon, the native JVM core and Android Clang');
 const work=fs.mkdtempSync(path.join(os.tmpdir(),'android-native-transfer-'));t.after(()=>fs.rmSync(work,{recursive:true,force:true}));
 const ids=path.join(work,'ids');fs.mkdirSync(ids,{mode:0o700});const guest=path.join(work,'libguest.so'),driver=path.join(work,'driver');
 const run=(program,args)=>{const result=spawnSync(program,args,{encoding:'utf8',timeout:15000,maxBuffer:2<<20,env:{...process.env,DYLD_LIBRARY_PATH:'',DYLD_FRAMEWORK_PATH:''}});assert.ifError(result.error);assert.equal(result.signal,null,result.stdout+result.stderr);assert.equal(result.status,0,result.stdout+result.stderr);return result.stdout;};
 const fixture=path.join(__dirname,'fixtures/android-native-transfer');
 run(clang,['-O2','-fPIC','-fno-stack-protector','-nostdlib','-shared','-Wl,--hash-style=both','-Wl,-z,max-page-size=16384',path.join(fixture,'guest.c'),'-o',guest]);
 run('clang',['-O2','-Wall','-Wextra','-Werror','-mmacosx-version-min=13.0',path.join(fixture,'driver.c'),'-o',driver]);
 if(process.env.KLEPTON_NATIVE_BASELINE)assert.match(run(driver,[path.resolve(process.env.KLEPTON_NATIVE_BASELINE),guest,ids,'baseline']),/BASELINE missing posix_madvise\/sendfile64; sendfile short-write returned=\d+ inputOffset=\d+ \(read-ahead loss=[1-9]\d*\)/);
 const output=run(driver,[path.resolve(core),guest,ids,'corrected']);
 for(const expected of [/PASS original Android posix_madvise preserves errno\/private bytes and returns Linux errors/,/PASS original Android sendfile names preserve explicit\/current offsets, file output, EOF and errors/,/PASS invalid\/read-only offset pointers return EFAULT before transfer without native fault/,/PASS nonblocking pipe partial mode=0/,/PASS nonblocking pipe partial mode=1/,/PASS nonblocking socket partial returned=\d+; EAGAIN leaves source offset unchanged/,/PASS actual Android-produced ELF calls corrected POSIX advice and Linux sendfile ABI/])assert.match(output,expected);
 t.diagnostic(output.trim());
});
