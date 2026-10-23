import test from 'node:test';import assert from 'node:assert/strict';import fs from 'node:fs';import os from 'node:os';import path from 'node:path';import {fileURLToPath}from'node:url';import {spawnSync}from'node:child_process';
import {adaptTranslator}from'../src/modules/android/native/converter-source.mjs';
test('oversized array payloads retain primitive bits, partial writes, handlers and monitors',t=>{
 const checkout=process.env.ANDROID_DEX2JVM_CHECKOUT,jdk=process.env.ANDROID_JAVA_HOME,overlay=process.env.ANDROID_CONVERTER_OVERLAY,python=process.env.ANDROID_PYTHON;
 if(!checkout||!jdk||!overlay||!python)return t.skip('Requires pinned converter, JDK21, converter overlay and Python environment');
 const output=fs.mkdtempSync(path.join(os.tmpdir(),'android-converter-arrays-'));t.after(()=>fs.rmSync(output,{recursive:true,force:true}));
 const fixture=name=>fileURLToPath(new URL('./helpers/'+name,import.meta.url));
 const run=(cmd,args)=>{const result=spawnSync(cmd,args,{encoding:'utf8'});assert.ifError(result.error);assert.equal(result.status,0,result.stdout+result.stderr);return result;};
 run(python,[fixture('android-converter-array-fixtures.py'),output]);
 const source=path.join(output,'Translator.java'),classes=path.join(output,'classes'),upstream=path.join(checkout,'build/dex2jvm.jar'),classpath=[overlay,upstream].join(path.delimiter);
 fs.writeFileSync(source,adaptTranslator(fs.readFileSync(path.join(checkout,'src/main/java/io/github/kksimp/dex2jvm/Translator.java'),'utf8')));
 run(path.join(jdk,'bin/javac'),['--release','21','-cp',classpath,'-d',classes,source,fixture('ArrayDataControl.java')]);
 const java=path.join(jdk,'bin/java'),fixed=[classes,classpath].join(path.delimiter);
 const large=run(java,['-Xverify:all','-cp',fixed,'ArrayDataControl',path.join(output,'fixtures/large'),path.join(output,'large')]);
 assert.equal((large.stdout.match(/PASS /g)||[]).length,8);
 run(java,['-Xverify:all','-cp',fixed,'ArrayDataControl',path.join(output,'fixtures/interface'),path.join(output,'interface'),'B']);
 run(java,['-Xverify:all','-cp',fixed,'ArrayDataControl',path.join(output,'fixtures/small'),path.join(output,'small-fixed')]);
 // Place the baseline converter before the harness directory; the latter also
 // holds the newly compiled Translator and must not shadow the baseline.
 run(java,['-Xverify:all','-cp',[classpath,classes].join(path.delimiter),'ArrayDataControl',path.join(output,'fixtures/small'),path.join(output,'small-baseline')]);
 for(const name of fs.readdirSync(path.join(output,'small-fixed')))assert.deepEqual(fs.readFileSync(path.join(output,'small-fixed',name)),fs.readFileSync(path.join(output,'small-baseline',name)),'Fitting class bytes changed: '+name);
});
