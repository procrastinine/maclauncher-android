const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),os=require('node:os'),path=require('node:path');
const {spawnSync}=require('node:child_process');
test('ELF image publication and retirement accept owned sub-page placement without relaxing syscall alignment',{timeout:30000},t=>{
 const core=process.env.KLEPTON_NATIVE_CORE,compiler=process.env.ANDROID_CLANG;
 if(process.platform!=='darwin'||process.arch!=='arm64'||!core||!compiler)return t.skip('Requires Apple Silicon, the native JVM core and Android Clang');
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-image-alignment-'));t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const fixture=path.resolve(__dirname,'fixtures/android-native'),ids=path.join(root,'ids');fs.mkdirSync(ids,{mode:0o700});
 const run=(command,args)=>{const result=spawnSync(command,args,{encoding:'utf8',timeout:15000,maxBuffer:1<<20});assert.ifError(result.error);assert.equal(result.signal,null,result.stdout+result.stderr);assert.equal(result.status,0,result.stdout+result.stderr);return result.stdout;};
 const guest=path.join(root,'libshifted.so');run(compiler,['-nostdlib','-shared','-Wl,--hash-style=both','-Wl,-z,max-page-size=4096','-Wl,-T,'+path.join(fixture,'image-alignment.ld'),path.join(fixture,'image-alignment-guest.S'),'-o',guest]);
 assert(fs.statSync(guest).size<32768);
 const probe=path.join(root,'probe');run('clang',['-O2','-Wall','-Wextra','-Werror','-mmacosx-version-min=13.0',path.join(fixture,'image-alignment.c'),'-o',probe]);
 const result=run(probe,[path.resolve(core),guest,ids]);
 assert.match(result,/PASS shifted 4KiB ELF publishes, executes and retires its covering 16KiB ownership pages/);
 assert.match(result,/PASS image claims preserve adjacent ownership, checked overflow and strict syscall alignment/);
});
