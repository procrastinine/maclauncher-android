const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),os=require('node:os'),path=require('node:path');
const {spawnSync}=require('node:child_process');
test('original Android regex records and complete native POSIX/GNU family execute from ARM64 ELF',{timeout:30000},t=>{
 const core=process.env.KLEPTON_NATIVE_CORE,compiler=process.env.ANDROID_CLANG,header=process.env.ANDROID_REGEX_HEADER;
 if(process.platform!=='darwin'||process.arch!=='arm64'||!core||!compiler||!header)return t.skip('Requires Apple Silicon, native JVM core, Android Clang and original Bionic regex header');
 const work=fs.mkdtempSync(path.join(os.tmpdir(),'android-regex-'));t.after(()=>fs.rmSync(work,{recursive:true,force:true}));
 const fixtures=path.resolve(__dirname,'fixtures/android-native'),ids=path.join(work,'ids');fs.mkdirSync(ids,{mode:0o700});fs.copyFileSync(header,path.join(work,'regex.h'));
 const run=(command,args)=>{const result=spawnSync(command,args,{encoding:'utf8',timeout:20000,maxBuffer:1<<20});assert.ifError(result.error);assert.equal(result.signal,null,result.stdout+result.stderr);assert.equal(result.status,0,result.stdout+result.stderr);return result.stdout;};
 run(compiler,['-shared','-nostdlib','-O1','-Wall','-Wextra','-Werror','-Wno-nullability-completeness','-Wl,--hash-style=both','-Wl,-soname,libregex-probe.so','-I'+work,path.join(fixtures,'regex-guest.c'),'-Wl,--no-as-needed','-lc','-o',path.join(work,'libregex-probe.so')]);
 const probe=path.join(work,'probe');run('clang',['-O2','-Wall','-Wextra','-Werror','-mmacosx-version-min=13.0',path.join(fixtures,'regex.c'),'-o',probe]);
 assert.match(run(probe,[path.resolve(core),work,ids]),/PASS original-header ARM64 ELF regex/);
});
