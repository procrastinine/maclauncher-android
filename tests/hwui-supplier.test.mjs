import fs from 'node:fs';import os from 'node:os';import path from 'node:path';import test from 'node:test';import assert from 'node:assert/strict';
import {buildHwuiSupplier} from '../src/modules/android/native/hwui/supplier.mjs';
test('ART HWUI producer is optional and carries the actual complete library and notices',async()=>{
 const work=fs.mkdtempSync(path.join(os.tmpdir(),'android-hwui-supplier-'));
 try {let calls=0;const notices=path.join(work,'notices'),library=path.join(work,'libhwui.so');fs.mkdirSync(notices);fs.writeFileSync(library,'complete-producer-output');
 const build=async options=>{calls++;assert.equal(options.work,path.join(work,'build/hwui'));assert.deepEqual(options.sources,{framework:'/original/framework'});assert.deepEqual(options.image,{root:'/original/image'});assert.equal(options.preparedNotices,notices);return {path:library};};
 assert.deepEqual(await buildHwuiSupplier({output:work},build),{});assert.equal(calls,0);
 await assert.rejects(buildHwuiSupplier({output:work,hwui:{notices:path.join(work,'absent')}},build));assert.equal(calls,0);
 assert.deepEqual(await buildHwuiSupplier({output:work,hwui:{notices,sources:{framework:'/original/framework'},image:{root:'/original/image'}}},build),{hwuiLibrary:library,hwuiNotices:notices});assert.equal(calls,1);
 const auto=await buildHwuiSupplier({output:work,hwui:{noticeInputs:{cargo:{registryCache:'/declared/registry'}}}},async options=>{assert.deepEqual(options.noticeInputs,{cargo:{registryCache:'/declared/registry'}});assert.equal(options.preparedNotices,undefined);return {path:library,notices};});assert.deepEqual(auto,{hwuiLibrary:library,hwuiNotices:notices});
 } finally {fs.rmSync(work,{recursive:true,force:true});}
});
