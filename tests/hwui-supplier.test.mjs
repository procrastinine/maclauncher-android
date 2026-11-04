import fs from 'node:fs';import os from 'node:os';import path from 'node:path';import test from 'node:test';import assert from 'node:assert/strict';
import {buildHwuiSupplier} from '../src/modules/android/native/hwui/supplier.mjs';
test('ART HWUI producer is optional and carries the actual complete library',async t=>{
 const work=fs.mkdtempSync(path.join(os.tmpdir(),'android-hwui-supplier-'));t.after(()=>fs.rmSync(work,{recursive:true,force:true}));
 const library=path.join(work,'libhwui.so');fs.writeFileSync(library,'complete-producer-output');let calls=0;
 const build=async options=>{calls++;assert.equal(options.work,path.join(work,'build/hwui'));assert.deepEqual(options.sources,{framework:'/original/framework'});assert.deepEqual(options.image,{root:'/original/image'});return {path:library};};
 assert.deepEqual(await buildHwuiSupplier({output:work},build),{});assert.equal(calls,0);
 assert.deepEqual(await buildHwuiSupplier({output:work,hwui:{sources:{framework:'/original/framework'},image:{root:'/original/image'}}},build),{hwuiLibrary:library});assert.equal(calls,1);
 await assert.rejects(buildHwuiSupplier({output:work,hwui:{}},async()=>({})),/returned no library/);
});
