import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {execFileSync,spawnSync} from 'node:child_process';
test('Original DexFile JNI declarations allow guest replacement and ordinary unregister restoration',t=>{
 const runtime=process.env.KLEPTON_NATIVE_RUNTIME,original=process.env.KLEPTON_AOSP_FRAMEWORK,clang=process.env.ANDROID_CLANG;
 if(process.platform!=='darwin'||process.arch!=='arm64'||!runtime||!original||!clang)return t.skip('Requires Apple Silicon, selected JVM runtime, original AOSP framework and Android clang');
 const home=process.env.JAVA_HOME||execFileSync('/usr/libexec/java_home',['-v','21'],{encoding:'utf8'}).trim();
 const root=fileURLToPath(new URL('../src/modules/android/native/',import.meta.url)),fixture=fileURLToPath(new URL('./fixtures/android-dex-native-registration/',import.meta.url));
 const directory=fs.mkdtempSync(path.join(os.tmpdir(),'android-dex-registration-'));t.after(()=>fs.rmSync(directory,{recursive:true,force:true}));
 const cp=['android-host.jar','android-framework.jar'].map(name=>path.resolve(runtime,name)).join(path.delimiter);
 const asm=JSON.parse(fs.readFileSync(path.join(root,'asm-binary.json'))).files.map(row=>path.join(process.env.ANDROID_TOOL_INPUTS||path.join(root,'tools'),row.path)).join(path.delimiter);
 const clean={...process.env};for(const key of Object.keys(clean))if(/^(DYLD_|KL_|JAVA_TOOL_OPTIONS$|JDK_JAVA_OPTIONS$|_JAVA_OPTIONS$|CLASSPATH$)/.test(key))delete clean[key];
 fs.mkdirSync(directory+'/ids',{mode:0o700});clean.KL_ANDROID_ID_DIRECTORY=directory+'/ids';clean.KL_ANDROID_NATIVE_STARTUP_ENVIRONMENT='1:';
 const run=(program,args)=>{const r=spawnSync(program,args,{encoding:'utf8',env:clean,timeout:30000,maxBuffer:2**20});assert.ifError(r.error);assert.equal(r.status,0,r.stdout+r.stderr);return r;};
 const java=path.join(home,'bin/java'),javac=path.join(home,'bin/javac');
 run(javac,['--release','21','-cp',asm,'-d',directory+'/tool',path.join(root,'framework/NativeBindings.java'),fixture+'/Declarations.java']);
 assert.match(run(java,['-cp',directory+'/tool'+path.delimiter+asm,'Declarations',original,directory+'/framework',directory+'/baseline-framework']).stdout,/PASS all original DexFile/);
 run(javac,['--release','21','-cp',cp,'-d',directory+'/app',fixture+'/Rebinder.java',fixture+'/Value.java']);
 run(path.join(home,'bin/jar'),['--create','--file',directory+'/app.jar','-C',directory+'/app','.']);
 run(javac,['--release','21','-cp',cp,'-d',directory+'/host',fixture+'/DexNativeRegistrationProbe.java']);
 fs.mkdirSync(directory+'/native');
 run(clang,['-shared','-nostdlib','-Wl,--hash-style=sysv','-Wl,-soname,libdexregistration.so','-I'+home+'/include','-I'+home+'/include/darwin',fixture+'/registration.c','-o',directory+'/native/libdexregistration.so']);
 const bridge=directory+'/dex-bridge.dylib';
 run('clang',['-dynamiclib','-arch','arm64','-mmacosx-version-min=13.0','-Wall','-Wextra','-Werror','-I'+home+'/include','-I'+home+'/include/darwin',path.join(root,'framework-dex.c'),'-o',bridge]);
 run('codesign',['--force','--sign','-',bridge]);
 const origin=directory+'/jni-warning-origin.dylib';
 run('clang',['-dynamiclib','-arch','arm64','-mmacosx-version-min=13.0','-Wall','-Wextra','-Werror','-I'+home+'/include','-I'+home+'/include/darwin',fixture+'/jni-warning-origin.c','-o',origin]);
 run('codesign',['--force','--sign','-',origin]);
 const options=['--add-exports=java.base/sun.net.www.protocol.jar=ALL-UNNAMED','--add-exports=java.base/sun.net.www=ALL-UNNAMED','--add-opens=java.base/jdk.internal.foreign=ALL-UNNAMED','--add-opens=java.base/java.util=ALL-UNNAMED','--add-opens=java.base/sun.security.ssl=ALL-UNNAMED','-Xcheck:jni','-Xverify:all','--add-exports=java.base/jdk.internal.ref=ALL-UNNAMED','--add-exports=java.base/sun.security.x509=ALL-UNNAMED','--add-exports=java.base/sun.security.jca=ALL-UNNAMED','--add-exports=java.base/sun.security.pkcs=ALL-UNNAMED','--add-opens=java.base/java.nio=ALL-UNNAMED'];
 const launch=(framework,selected,name)=>run(java,[...options,'-cp',[directory+'/host',directory+(framework?'/framework':'/baseline-framework'),cp].join(path.delimiter),'DexNativeRegistrationProbe',path.resolve(runtime),directory+'/app.jar',directory+'/'+name,selected,directory+'/native',origin]);
 assert.match(launch(false,'-','baseline').stdout,/PASS original runtime rejects replacement/);
 const result=launch(true,bridge,'candidate');assert.match(result.stdout,/PASS original DexFile native registration/);
 const publisher=fs.realpathSync(path.join(runtime,'native/libandroid_runtime.dylib'));
 let caller=null,upstream=0;
 for(const line of result.stdout.split('\n')){
  if(line.startsWith('JNI-CALL-ORIGIN '))caller=line.slice('JNI-CALL-ORIGIN '.length);
  else if(line==='JNI-CALL-END')caller=null;
  else if(line.includes('WARNING in native method')){
   assert.ok(caller&&caller!=='unknown','Every JNI warning has an actual native caller library');
   assert.equal(fs.realpathSync(caller),publisher,'JNI warning from owned code or another library: '+caller);
   upstream++;t.diagnostic('Upstream CheckJNI defect in '+path.basename(caller)+': '+line);
  }
 }
 assert.doesNotMatch(result.stderr,/WARNING in native method/,'JNI warning without native-library attribution');
 assert.doesNotMatch(result.stdout+result.stderr,/FATAL ERROR in native method/);
 t.diagnostic(upstream+' publisher warnings recorded; no owned or unattributed JNI warning');
});
