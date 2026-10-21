import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {publishNativeBinary} from '../src/modules/android/native/upstream/native-paths.mjs';

test('signed native publication replaces changed inodes and preserves active readers and no-op identity',{timeout:30000},t=>{
 if(process.platform!=='darwin')return t.skip('Requires the macOS native linker and signing tools');
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-native-publication-'));t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const source=path.join(root,'program.c');fs.writeFileSync(source,'#include <stdio.h>\nint main(void){printf("%d\\n",VALUE);return 0;}\n');
 const first=path.join(root,'first'),second=path.join(root,'second'),output=path.join(root,'published/program');
 for(const [file,value]of[[first,11],[second,22]])execFileSync('clang',['-O2','-mmacosx-version-min=13.0','-DVALUE='+value,source,'-o',file],{stdio:'pipe'});
 const run=()=>execFileSync(output,[],{encoding:'utf8'}).trim();
 publishNativeBinary(first,output);assert.equal(run(),'11');
 const original=fs.readFileSync(output),stat=fs.statSync(output),reader=fs.openSync(output,'r');t.after(()=>fs.closeSync(reader));
 publishNativeBinary(second,output);assert.notEqual(fs.statSync(output).ino,stat.ino);assert.equal(run(),'22');
 const retained=Buffer.alloc(original.length);assert.equal(fs.readSync(reader,retained,0,retained.length,0),retained.length);assert.deepEqual(retained,original);
 const unchanged=fs.statSync(output);publishNativeBinary(second,output);const after=fs.statSync(output);
 assert.equal(after.ino,unchanged.ino);assert.equal(after.mtimeMs,unchanged.mtimeMs);assert.equal(after.mode,unchanged.mode);assert.equal(run(),'22');
 fs.chmodSync(second,0o744);publishNativeBinary(second,output);assert.notEqual(fs.statSync(output).ino,after.ino);assert.equal(fs.statSync(output).mode&0o777,0o744);assert.equal(fs.fstatSync(reader).mode&0o777,stat.mode&0o777);assert.equal(run(),'22');
 const ordinary=fs.statSync(output);fs.chmodSync(second,0o4744);publishNativeBinary(second,output);
 assert.equal(fs.statSync(output).mode&0o7777,0o744);assert.equal(fs.statSync(output).ino,ordinary.ino);
 fs.chmodSync(output,0o4744);const special=fs.statSync(output);publishNativeBinary(second,output);
 assert.notEqual(fs.statSync(output).ino,special.ino);assert.equal(fs.statSync(output).mode&0o7777,0o744);
 execFileSync('codesign',['--verify','--strict',output],{stdio:'pipe'});
 const entries=fs.readdirSync(path.dirname(output));assert.deepEqual(entries,['program']);
});
