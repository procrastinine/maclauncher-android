import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {addLicense,includeLicenses,addLicenses} from '../src/modules/android/native/upstream/licenses.mjs';
test('licence functions never throw for unavailable, unreadable and unwritable inputs',t=>{
 t.mock.method(console,'log',()=>{});
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-collection-'));
 t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const file=path.join(root,'file');fs.writeFileSync(file,'input');
 for(const value of [undefined,null,42,{},root,file,path.join(root,'missing')]){
  assert.doesNotThrow(()=>addLicense(value,'component',value));
  assert.doesNotThrow(()=>includeLicenses(value,value));
  assert.doesNotThrow(()=>addLicenses(value,'component',value));
 }
});

test('licence functions contain malformed arguments and failures while warning',t=>{
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-warning-'));
 t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const file=path.join(root,'LICENSE');fs.writeFileSync(file,'input');
 const hostile={toString(){throw Error('argument conversion');}};
 t.mock.method(console,'log',()=>{throw Error('warning output');});
 for(const value of [Symbol('component'),hostile]){
  assert.doesNotThrow(()=>addLicense(root,value,file));
  assert.doesNotThrow(()=>includeLicenses(root,value));
  assert.doesNotThrow(()=>addLicenses(root,'component',value));
 }
 assert.doesNotThrow(()=>addLicenses(root,'component',Buffer.from(root)));
 assert.doesNotThrow(()=>addLicenses(root,'component',new URL('file://'+root)));
});
