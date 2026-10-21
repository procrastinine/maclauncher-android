import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';

test('real ARM ELF metadata retains long names and rejects malformed string tables',t=>{
 const jdk=process.env.JAVA_HOME,compiler=process.env.ANDROID_CLANG;
 if(!jdk||!compiler)return t.skip('Requires a JDK and the Android NDK ARM64 compiler');
 const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
 const directory=fs.mkdtempSync(path.join(os.tmpdir(),'android-elf-symbols-'));
 t.after(()=>fs.rmSync(directory,{recursive:true,force:true}));
 const run=(command,args)=>{const result=spawnSync(command,args,{encoding:'utf8',timeout:30000});assert.ifError(result.error);assert.equal(result.status,0,result.stdout+result.stderr);return result.stdout;};
 const names=['ordinary_'+ 'x'.repeat(6300),'Java_fixture_Long_'+ 'm'.repeat(6200),'Java_fixture_Short_nativeCall'];
 const assembly=path.join(directory,'names.S');
 fs.writeFileSync(assembly,names.map(name=>`.global ${name}\n.type ${name},%function\n${name}:\n bx lr\n`).join('\n'));
 const arm32=path.join(path.dirname(compiler),'armv7a-linux-androideabi28-clang');
 const files=[];
 for(const [tool,wide] of [[compiler,true],[arm32,false]]){
  assert.ok(fs.existsSync(tool),'Android compiler is unavailable: '+tool);
  const source=path.join(directory,wide?'names64.S':'names32.S');
  fs.writeFileSync(source,fs.readFileSync(assembly,'utf8').replaceAll('bx lr',wide?'ret':'bx lr'));
  const file=path.join(directory,wide?'libnames64.so':'libnames32.so');
  run(tool,['-nostdlib','-shared','-Wl,--hash-style=both',source,'-o',file]);files.push(file);
 }
 const fixture=path.join(root,'tests/fixtures/android-elf-symbol-metadata/SymbolNamesProbe.java');
 const source=path.join(root,'src/modules/android/native/java/maclauncher/android/bridge/ElfNativeExports.java');
 run(path.join(jdk,'bin/javac'),['--release','21','-d',directory,source,fixture]);
 const output=run(path.join(jdk,'bin/java'),['-cp',directory,'maclauncher.android.bridge.SymbolNamesProbe',...files]);
 assert.match(output,/PASS real ELF32\/ELF64 long ordinary and JNI names; bounds, overflow and termination controls/);
 t.diagnostic(output.trim());
});
