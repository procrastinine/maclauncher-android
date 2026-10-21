const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),os=require('node:os'),path=require('node:path');
const {spawnSync}=require('node:child_process');
test('named ARM64 dlsym searches only its original dependency closure and actual platform adapters',{timeout:30000},t=>{
 const core=process.env.KLEPTON_NATIVE_CORE,compiler=process.env.ANDROID_CLANG;
 if(process.platform!=='darwin'||process.arch!=='arm64'||!core||!compiler)return t.skip('Requires Apple Silicon, the native JVM core and Android Clang');
 const work=fs.mkdtempSync(path.join(os.tmpdir(),'android-symbol-scope-'));t.after(()=>fs.rmSync(work,{recursive:true,force:true}));
 const fixtures=path.resolve(__dirname,'fixtures/android-native'),ids=path.join(work,'ids');fs.mkdirSync(ids,{mode:0o700});
 const run=(command,args)=>{const result=spawnSync(command,args,{encoding:'utf8',timeout:20000,maxBuffer:1<<20});assert.ifError(result.error);assert.equal(result.signal,null,result.stdout+result.stderr);assert.equal(result.status,0,result.stdout+result.stderr);return result.stdout;};
 function library(name,define,deps=[]){run(compiler,['-shared','-nostdlib','-O1','-Wl,--hash-style=both','-Wl,-soname,'+name,'-D'+define,path.join(fixtures,'symbol-scope-guest.c'),'-L'+work,'-Wl,--no-as-needed',...deps,'-o',path.join(work,name)]);}
 library('libdeep.so','DEEP_IMAGE');library('liba.so','DEPENDENCY_A',['-ldeep']);library('libb.so','DEPENDENCY_B');
 library('libunrelated.so','UNRELATED_IMAGE');library('libbare.so','BARE_IMAGE');library('libroot.so','ROOT_IMAGE',['-la','-lb','-lc']);
 const probe=path.join(work,'probe');run('clang',['-O2','-Wall','-Wextra','-Werror','-mmacosx-version-min=13.0',path.join(fixtures,'symbol-scope.c'),'-o',probe]);
 assert.match(run(probe,[path.resolve(core),work,ids]),/PASS named ELF handle own\/dependency\/BFS precedence, real libc adapters/);
});
