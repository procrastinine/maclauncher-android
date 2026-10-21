import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';

test('WebKit failure callbacks retain NSError and original request URLs without opening a browser',{timeout:45000},t=>{
 const runtime=process.env.KLEPTON_NATIVE_RUNTIME,jdk=process.env.JAVA_HOME;
 if(process.platform!=='darwin'||!runtime||!jdk)return t.skip('Requires macOS, Java 21 and the complete Android runtime archives');
 const temporary=fs.mkdtempSync(path.join(os.tmpdir(),'android-webkit-errors-'));
 t.after(()=>fs.rmSync(temporary,{recursive:true,force:true}));
 const native=fileURLToPath(new URL('../src/modules/android/native/',import.meta.url));
 const fixtures=fileURLToPath(new URL('./fixtures/android-webkit-errors/',import.meta.url));
 const run=(command,args)=>{
  const result=spawnSync(command,args,{encoding:'utf8',timeout:30000,maxBuffer:2<<20});
  assert.ifError(result.error);assert.equal(result.signal,null,result.stdout+result.stderr);
  assert.equal(result.status,0,result.stdout+result.stderr);return result.stdout;
 };
 const probe=path.join(temporary,'navigation');
 run('clang++',['-std=c++20','-fobjc-arc','-Wall','-Wextra','-Werror','-I'+native,path.join(fixtures,'navigation.mm'),'-framework','Foundation','-o',probe]);
 const results=run(probe,[]),records=JSON.parse(results);
 assert.equal(records.length,6);
 const errors=path.join(temporary,'errors.json');fs.writeFileSync(errors,results);
 // Parse the full physical owner too; no browser, GUI or game process starts.
 run('clang++',['-std=c++20','-fobjc-arc','-Werror=return-type','-fsyntax-only','-I'+native,path.join(native,'webkit-platform.mm')]);
 const classes=path.join(temporary,'classes');fs.mkdirSync(classes);
 const cp=[classes,path.resolve(runtime,'android-host.jar'),path.resolve(runtime,'android-framework.jar')].join(path.delimiter);
 run(path.join(jdk,'bin/javac'),['--release','21','-cp',cp,'-d',classes,path.join(native,'java/maclauncher/android/web/WebKitView.java'),path.join(fixtures,'WebKitErrorProbe.java')]);
 const output=run(path.join(jdk,'bin/java'),['-Xverify:all','-cp',cp,'WebKitErrorProbe',errors]);
 assert.match(output,/PASS actual NSError\/request URLs reach original Android error callbacks without nulls or document-title corruption/);
 t.diagnostic(output.trim());
});
