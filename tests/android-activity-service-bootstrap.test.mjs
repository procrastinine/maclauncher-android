import test from 'node:test';import assert from 'node:assert/strict';import fs from 'node:fs';import os from 'node:os';import path from 'node:path';import {fileURLToPath} from 'node:url';import {spawnSync} from 'node:child_process';

test('the actual activity endpoint precedes original ContextImpl permission checks',{timeout:45000},t=>{
 const runtime=process.env.KLEPTON_NATIVE_RUNTIME,jdk=process.env.JAVA_HOME;
 if(process.platform!=='darwin'||!runtime||!jdk)return t.skip('Requires macOS, Java21 and the complete Android runtime archives');
 const temporary=fs.mkdtempSync(path.join(os.tmpdir(),'android-activity-bootstrap-'));t.after(()=>fs.rmSync(temporary,{recursive:true,force:true}));
 const native=fileURLToPath(new URL('../src/modules/android/native/',import.meta.url)),fixtures=fileURLToPath(new URL('./fixtures/android-activity-bootstrap/',import.meta.url));
 const classes=path.join(temporary,'classes'),ids=path.join(temporary,'ids');fs.mkdirSync(classes);fs.mkdirSync(ids,{mode:0o700});
 const cp=[classes,path.resolve(runtime,'android-host.jar'),path.resolve(runtime,'android-framework.jar')].join(path.delimiter);
 const options=['--add-exports=java.base/jdk.internal.ref=ALL-UNNAMED','--add-exports=java.base/sun.net.www.protocol.jar=ALL-UNNAMED','--add-exports=java.base/sun.net.www=ALL-UNNAMED','--add-exports=java.base/sun.security.x509=ALL-UNNAMED','--add-exports=java.base/sun.security.jca=ALL-UNNAMED','--add-exports=java.base/sun.security.pkcs=ALL-UNNAMED','--add-opens=java.base/sun.security.ssl=ALL-UNNAMED','--add-opens=java.base/java.util=ALL-UNNAMED','--add-opens=java.base/java.nio=ALL-UNNAMED','--add-opens=java.base/jdk.internal.foreign=ALL-UNNAMED'];
 const run=(tool,args)=>{const result=spawnSync(path.join(jdk,'bin',tool),args,{encoding:'utf8',timeout:20000,maxBuffer:2<<20,env:{...process.env,KL_ANDROID_ID_DIRECTORY:ids}});assert.ifError(result.error);assert.equal(result.signal,null,result.stdout+result.stderr);assert.equal(result.status,0,result.stdout+result.stderr);return result;};
 run('javac',['--release','21','-cp',cp,'-d',classes,...['HostApplication','HostActivityService'].map(name=>path.join(native,'java/maclauncher/android/framework',name+'.java')),path.join(fixtures,'ActivityServiceBootstrapProbe.java')]);
 for(const mode of ['bootstrap','precontext']){
  const result=run('java',[...options,'-Xverify:all','-cp',cp,'maclauncher.android.framework.ActivityServiceBootstrapProbe',path.resolve(runtime),path.join(temporary,mode),path.join(fixtures,'AndroidManifest.xml.bin'),mode]);
  assert.match(result.stdout,/PASS .*original .*permission|PASS original ContextImpl first permission lookup/);
  assert.doesNotMatch(result.stderr,/Missing ActivityManager; assuming/,'the original permission client must resolve its activity service');
  t.diagnostic(result.stdout.trim());
 }
});
