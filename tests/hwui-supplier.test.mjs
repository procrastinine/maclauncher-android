import fs from 'node:fs';import os from 'node:os';import path from 'node:path';import test from 'node:test';import assert from 'node:assert/strict';
import {buildHwuiSupplier} from '../src/modules/android/native/hwui/supplier.mjs';
test('ART HWUI producer is optional and carries the actual complete library and licenses',async()=>{
 const work=fs.mkdtempSync(path.join(os.tmpdir(),'android-hwui-supplier-'));
 try {let calls=0;const licenses=path.join(work,'licenses'),library=path.join(work,'libhwui.so');fs.mkdirSync(licenses);fs.writeFileSync(library,'complete-producer-output');
 const build=async options=>{calls++;assert.equal(options.work,path.join(work,'build/hwui'));assert.deepEqual(options.sources,{framework:'/original/framework'});assert.deepEqual(options.image,{root:'/original/image'});assert.equal(options.preparedLicenses,licenses);return {path:library};};
 assert.deepEqual(await buildHwuiSupplier({output:work},build),{});assert.equal(calls,0);
 assert.deepEqual(await buildHwuiSupplier({output:work,hwui:{licenses:path.join(work,'absent')}},async()=>({path:library})),{hwuiLibrary:library,hwuiLicenses:path.join(work,'absent')});assert.equal(calls,0);
 assert.deepEqual(await buildHwuiSupplier({output:work,hwui:{licenses,sources:{framework:'/original/framework'},image:{root:'/original/image'}}},build),{hwuiLibrary:library,hwuiLicenses:licenses});assert.equal(calls,1);
 const auto=await buildHwuiSupplier({output:work,hwui:{licenseInputs:{cargo:{registryCache:'/declared/registry'}}}},async options=>{assert.deepEqual(options.licenseInputs,{cargo:{registryCache:'/declared/registry'}});assert.equal(options.preparedLicenses,undefined);return {path:library,licenses};});assert.deepEqual(auto,{hwuiLibrary:library,hwuiLicenses:licenses});
 } finally {fs.rmSync(work,{recursive:true,force:true});}
});
