const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),os=require('node:os'),path=require('node:path');
const {spawnSync}=require('node:child_process');
test('JVM executable protection preserves native mapping owners and guest mapping provenance',{timeout:30000},t=>{
 const runtime=process.env.KLEPTON_NATIVE_RUNTIME,core=process.env.KLEPTON_NATIVE_CORE||(runtime&&path.resolve(runtime,'native/libklepton-jvm.dylib'));
 if(process.platform!=='darwin'||process.arch!=='arm64'||!core)return t.skip('Requires Apple Silicon and the native JVM runtime');
 const temporary=fs.mkdtempSync(path.join(os.tmpdir(),'android-memory-domain-'));t.after(()=>fs.rmSync(temporary,{recursive:true,force:true}));
 const ids=path.join(temporary,'ids');fs.mkdirSync(ids,{mode:0o700});const binary=path.join(temporary,'probe');
 const run=(program,args)=>{const result=spawnSync(program,args,{encoding:'utf8',timeout:15000,maxBuffer:2<<20,env:{...process.env,DYLD_LIBRARY_PATH:'',DYLD_FRAMEWORK_PATH:''}});assert.ifError(result.error);assert.equal(result.signal,null,result.stdout+result.stderr);assert.equal(result.status,0,result.stdout+result.stderr);return result.stdout;};
 run('clang',['-O2','-Wall','-Wextra','-Werror','-mmacosx-version-min=13.0',path.resolve('tests/fixtures/android-guest-executable-memory/domains.c'),'-o',binary]);
 for(const mode of ['host-text','host-stack','host-jit'])assert.match(run(binary,[path.resolve(core),ids,mode]),new RegExp('PASS '+mode+' retains original Darwin result, errno and mapping identity'));
 assert.match(run(binary,[path.resolve(core),ids,'guest-ranges']),/PASS guest mmap, fixed replacement, partial retirement, mremap move\/shrink\/alias and fork retain executable ownership/);
 assert.match(run(binary,[path.resolve(core),ids,'failed-fixed']),/PASS failed executable fixed replacement retires prior guest ownership/);
});
function buildLinuxHandler(run,ndk,temporary){
 const handler=path.join(temporary,'linux-signal.so'),instructions=path.join(temporary,'linux-signal.bin');
 run(path.join(ndk,'aarch64-linux-android35-clang'),['-O2','-fPIC','-ffreestanding','-fno-stack-protector','-fno-unwind-tables','-nostdlib','-shared',path.resolve('tests/fixtures/android-guest-executable-memory/linux-signal.c'),'-o',handler]);
 run(path.join(ndk,'llvm-objcopy'),['--only-section=.text','-O','binary',handler,instructions]);
 return {handler,instructions};
}

test('Native executable-memory ABI reuses original AIM JIT allocation and retirement',{timeout:180000},async t=>{
 const source=process.env.ANDROID_AIM_MEMORY_SOURCE,target=process.env.ANDROID_AIM_CARGO_TARGET,ndk=process.env.ANDROID_NDK_TOOLS;
 if(process.platform!=='darwin'||process.arch!=='arm64'||!source||!target||!ndk)return t.skip('Requires Apple Silicon, Android NDK and the original AIM projection with the executable-memory ABI');
 const temporary=fs.mkdtempSync(path.join(os.tmpdir(),'android-guest-memory-'));t.after(()=>fs.rmSync(temporary,{recursive:true,force:true}));
 const run=(command,args,timeout=15000)=>{const result=spawnSync(command,args,{encoding:'utf8',timeout,maxBuffer:4<<20});assert.ifError(result.error);assert.equal(result.signal,null,result.stdout+result.stderr);assert.equal(result.status,0,result.stdout+result.stderr);return result;};
 run('cargo',['rustc','--offline','--release','-j','2','--manifest-path',path.resolve(source,'Cargo.toml'),'--target-dir',path.resolve(target),'-p','aim-linux-abi','--lib','--crate-type','cdylib'],150000);
 const producer=path.resolve(target,'release/libaim_linux_abi.dylib');assert.ok(fs.existsSync(producer));
 const {publishNativeBinary}=await import('../src/modules/android/native/upstream/native-paths.mjs');
 const library=path.join(temporary,'libandroid-guest-memory.dylib');
 publishNativeBinary(producer,library,{installName:'@rpath/libandroid-guest-memory.dylib',rpaths:[]});
 assert.equal(run('otool',['-D',library]).stdout.trim().split('\n').at(-1),'@rpath/libandroid-guest-memory.dylib');
 const executable=path.join(temporary,'probe');
 const {instructions}=buildLinuxHandler(run,ndk,temporary);
 run('clang',['-O2','-mmacosx-version-min=13.0','-I',path.resolve('src/modules/android/native/executable-memory'),path.resolve('tests/fixtures/android-guest-executable-memory/main.c'),'-o',executable]);
 const result=run(executable,[library,instructions]);
 assert.match(result.stdout,/PASS original AIM executable-memory owner preserves bytes, executes 32 revisions, retires JIT mappings and keeps native errno/);
 assert.match(result.stdout,/PASS actual native JIT write, execute-resume and self-modifying-store faults reuse original AIM logic; handled=\d+ forwarded=1/);
 assert.match(result.stdout,/PASS original Linux signal frame executes Android NDK handler and restores registers and mask/);
 t.diagnostic(result.stdout.trim());
});

test('JVM native imports use the original executable-memory owner',{timeout:30000},t=>{
 const runtime=process.env.KLEPTON_NATIVE_RUNTIME,core=process.env.KLEPTON_NATIVE_CORE||(runtime&&path.resolve(runtime,'native/libklepton-jvm.dylib')),ndk=process.env.ANDROID_NDK_TOOLS;
 if(process.platform!=='darwin'||process.arch!=='arm64'||!core||!ndk)return t.skip('Requires Apple Silicon, Android NDK and the native JVM runtime');
 const temporary=fs.mkdtempSync(path.join(os.tmpdir(),'android-jvm-memory-'));t.after(()=>fs.rmSync(temporary,{recursive:true,force:true}));
 const run=(command,args)=>{const result=spawnSync(command,args,{encoding:'utf8',timeout:15000,maxBuffer:4<<20});assert.ifError(result.error);assert.equal(result.signal,null,result.stdout+result.stderr);assert.equal(result.status,0,result.stdout+result.stderr);return result;};
 const executable=path.join(temporary,'probe');
 const {handler}=buildLinuxHandler(run,ndk,temporary);
 const identities=path.join(temporary,'identities');fs.mkdirSync(identities,{mode:0o700});
 run('clang',['-O2','-mmacosx-version-min=13.0',path.resolve('tests/fixtures/android-guest-executable-memory/jvm.c'),'-o',executable]);
 const result=run(executable,[path.resolve(core),handler,identities]);
 assert.match(result.stdout,/PASS actual JVM mmap\/mprotect imports execute Android JIT and retain native signal ownership/);
 assert.match(result.stdout,/PASS actual ELF SA_SIGINFO registration preserves Android NDK register and signal-mask edits/);
 assert.match(result.stdout,/PASS final JVM code publication preserves guest TLS, x18, PC-relative addresses, Linux SVC and veneer retirement/);
 assert.match(result.stdout,/PASS failed code preparation preserves original bytes, writable protection and native errno/);
 t.diagnostic(result.stdout.trim());
});
