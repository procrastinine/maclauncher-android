const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),os=require('node:os'),path=require('node:path');
const {spawnSync}=require('node:child_process');
test('Original initial-exec ELF TLS uses the retained Android TP and agrees with dynamic TLS',t=>{
 const core=process.env.KLEPTON_NATIVE_CORE,clang=process.env.ANDROID_CLANG;
 if(process.platform!=='darwin'||process.arch!=='arm64'||!core||!clang)return t.skip('Requires Apple Silicon, the changed native core and Android Clang');
 const work=fs.mkdtempSync(path.join(os.tmpdir(),'android-elf-initial-exec-'));t.after(()=>fs.rmSync(work,{recursive:true,force:true}));
 const ids=path.join(work,'process-ids');fs.mkdirSync(ids,{mode:0o700});
 const env={...process.env,KL_ANDROID_ID_DIRECTORY:ids},fixture=path.resolve(__dirname,'fixtures/android-native');
 const run=(file,args)=>{const r=spawnSync(file,args,{env,encoding:'utf8',timeout:30000,maxBuffer:2<<20});assert.ifError(r.error);assert.equal(r.signal,null,r.stdout+r.stderr);assert.equal(r.status,0,r.stdout+r.stderr);return r.stdout;};
 const sources=['elf-tls-dependency','elf-tls-initial-exec-guest','elf-tls-initial-exec-peer','elf-tls-initial-exec-too-large'];
 const libraries=sources.map((name,index)=>{
  const output=path.join(work,name+'.so');
  run(clang,['-shared','-nostdlib','-fPIC','-fno-emulated-tls','-fno-stack-protector','-O2','-Wl,--hash-style=both','-Wl,-z,max-page-size=16384',...(index===1||index===3?['-ftls-model=initial-exec']:[]),path.join(fixture,name+'.c'),'-o',output]);return output;
 });
 // The ordinary NDK compiler emits real IE relocations; no guest instruction
 // or relocation is synthesized or edited by the fixture.
 const readelf=path.join(path.dirname(fs.realpathSync(clang)),'llvm-readelf');
 const relocations=run(readelf,['--relocs',libraries[1]]);assert.match(relocations,/R_AARCH64_TLS_TPREL/);
 assert.match(run(readelf,['--relocs',libraries[2]]),/R_AARCH64_TLSDESC/);
 const binary=path.join(work,'probe');run('clang',['-O2','-Wall','-Wextra','-Werror',path.join(fixture,'elf-tls-initial-exec.c'),'-o',binary]);
 assert.match(run(binary,[path.resolve(core),...libraries]),/PASS original initial-exec ELF TLS shares real TP-relative storage with dynamic TLS on existing\/new threads, late destructors and fork/);
});
