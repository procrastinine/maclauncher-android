const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),os=require('node:os'),path=require('node:path');
const{spawnSync}=require('node:child_process');
test('original Android fallocate names preserve partial blocks, EOF, descriptors and native errors',{timeout:30000},t=>{
 const core=process.env.KLEPTON_NATIVE_CORE,clang=process.env.ANDROID_CLANG;
 if(process.platform!=='darwin'||process.arch!=='arm64'||!core||!clang)return t.skip('Requires Apple Silicon, the native JVM core and Android Clang');
 const work=fs.mkdtempSync(path.join(os.tmpdir(),'android-native-allocation-'));t.after(()=>fs.rmSync(work,{recursive:true,force:true}));
 const ids=path.join(work,'ids');fs.mkdirSync(ids,{mode:0o700});const guest=path.join(work,'libguest.so'),driver=path.join(work,'driver');
 const run=(program,args)=>{const result=spawnSync(program,args,{encoding:'utf8',timeout:15000,maxBuffer:2<<20,env:{...process.env,DYLD_LIBRARY_PATH:'',DYLD_FRAMEWORK_PATH:''}});assert.ifError(result.error);assert.equal(result.signal,null,result.stdout+result.stderr);assert.equal(result.status,0,result.stdout+result.stderr);return result.stdout;};
 const fixture=path.join(__dirname,'fixtures/android-native-transfer');
 run(clang,['-O2','-fPIC','-fno-stack-protector','-nostdlib','-shared','-Wl,--hash-style=both','-Wl,-z,max-page-size=16384',path.join(fixture,'guest.c'),'-o',guest]);
 run('clang',['-O2','-Wall','-Wextra','-Werror','-mmacosx-version-min=13.0',path.join(fixture,'driver.c'),'-o',driver]);
 const output=run(driver,[path.resolve(core),guest,ids,'allocation']);
 for(const name of ['fallocate','fallocate64'])for(let c=0;c<6;c++)assert.match(output,new RegExp(`PASS ${name} punch case=${c} block=\\d+ preserves exact bytes, inode, size, flags and offset`));
 assert.match(output,/PASS unsupported mode, append and readonly return Linux errors without mutation/);
 assert.match(output,/PASS original Android fallocate\/fallocate64 unaligned and EOF-clipped hole punching/);t.diagnostic(output.trim());
});
