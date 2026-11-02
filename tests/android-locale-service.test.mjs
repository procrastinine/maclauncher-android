import { createRequire as fixtureRequire } from 'node:module';
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawnSync} from 'node:child_process';

// The supplied full Android framework and native runtime are explicit inputs.
// This check starts no display, Android service process or application session.
test('original LocaleManager Binder, package configuration and lifetime',{timeout:60000},t=>{
 const runtime=process.env.KLEPTON_NATIVE_RUNTIME,jdk=process.env.JAVA_HOME;
 if(process.platform!=='darwin'||!runtime||!jdk)return t.skip('Requires macOS, Java21 and the complete Android runtime');
 const temporary=fs.mkdtempSync(path.join(os.tmpdir(),'android-locale-service-'));t.after(()=>fs.rmSync(temporary,{recursive:true,force:true}));
 const native=path.resolve(import.meta.dirname,'../src/modules/android/native'),fixture=path.join(import.meta.dirname,'fixtures/android-locale-service');
 const classes=path.join(temporary,'classes'),data=path.join(temporary,'data'),ids=path.join(temporary,'ids');fs.mkdirSync(classes);fs.mkdirSync(data);fs.mkdirSync(ids,{mode:0o700});
 const env={...process.env,KL_ANDROID_ID_DIRECTORY:ids};for(const key of Object.keys(env))if(/^(DYLD_|RESHADE_)/.test(key)||['JAVA_TOOL_OPTIONS','JDK_JAVA_OPTIONS','_JAVA_OPTIONS','CLASSPATH'].includes(key))delete env[key];
 const cp=[classes,path.resolve(runtime,'android-host.jar'),path.resolve(runtime,'android-framework.jar'),path.join(native,'tools/*')].join(path.delimiter);
 const run=(command,args)=>{const r=spawnSync(command,args,{env,encoding:'utf8',timeout:30000,maxBuffer:4<<20});assert.ifError(r.error);assert.equal(r.signal,null,r.stdout+r.stderr);assert.equal(r.status,0,r.stdout+r.stderr);return r;};
 const sources=['framework/ServicePortability.java','java/com/android/server/wm/HostPackageConfiguration.java','java/com/android/server/locales/HostLocaleService.java','java/maclauncher/android/framework/HostLocaleConfiguration.java','java/maclauncher/android/framework/HostApplication.java','java/maclauncher/android/framework/HostActivityManager.java','java/maclauncher/android/framework/HostActivityService.java','java/maclauncher/android/framework/HostSystemServices.java','java/maclauncher/android/PlatformServiceHost.java','framework/PendingIntentCalls.java'].map(p=>path.join(native,p));
 run(path.join(jdk,'bin/javac'),['--release','21','-cp',cp,'-d',classes,...sources,...['LocaleBindings.java','LocaleServiceProbe.java','RuntimeFixture.java'].map(p=>path.join(fixture,p))]);
 run(path.join(jdk,'bin/java'),['-cp',cp,'LocaleBindings',path.resolve(runtime,'android-framework.jar'),classes]);
 const library=path.join(temporary,'locale-properties.dylib');run(process.env.CLANG||'clang',['-dynamiclib','-Wall','-Wextra','-Werror','-I'+path.join(jdk,'include'),'-I'+path.join(jdk,'include/darwin'),path.join(fixture,'locale-properties.c'),'-o',library]);
 const options=['--add-opens=java.base/jdk.internal.foreign=ALL-UNNAMED','--add-opens=java.base/java.nio=ALL-UNNAMED','--add-opens=java.base/java.util=ALL-UNNAMED','--add-exports=java.base/jdk.internal.ref=ALL-UNNAMED','--add-exports=java.base/sun.net.www.protocol.jar=ALL-UNNAMED','--add-exports=java.base/sun.net.www=ALL-UNNAMED','--add-exports=java.base/sun.security.x509=ALL-UNNAMED','--add-exports=java.base/sun.security.jca=ALL-UNNAMED','--add-exports=java.base/sun.security.pkcs=ALL-UNNAMED','--add-opens=java.base/sun.security.ssl=ALL-UNNAMED'];
 const result=run(path.join(jdk,'bin/java'),[...options,'-Xverify:all','-cp',cp,'maclauncher.android.framework.LocaleServiceProbe',path.resolve(runtime),data,fixtureRequire(import.meta.url)('./helpers/manifest-fixtures').file(t,'android-activity-bootstrap/AndroidManifest'),library]);
 assert.match(result.stdout,/PASS full original LocaleManager:/);
 assert.doesNotMatch(result.stderr,/failed to commit locale setter info|Couldn't create directory for SharedPreferences|Exception in thread/);
 t.diagnostic(result.stdout.trim());
});
