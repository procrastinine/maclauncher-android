import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {packageArtAdapter} from '../src/modules/android/art/package.mjs';

test('standalone ART packaging checks producer identity without creating a stage',t=>{
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-package-inputs-'));
 t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const supplierRoot=path.join(root,'supplier'),commonRoot=path.join(root,'common');
 fs.mkdirSync(supplierRoot);fs.mkdirSync(commonRoot);
 fs.writeFileSync(path.join(supplierRoot,'runtime.json'),'{}');
 fs.writeFileSync(path.join(commonRoot,'runtime.json'),JSON.stringify({profile:{javaMajor:21}}));
 const options={version:'0.2.0',supplierRoot,commonRoot,extractor:path.join(root,'extractor'),sourceArchive:path.join(root,'source.tar.gz'),output:path.join(root,'uncreated/adapter')};
 const metadata=Object.fromEntries(['fileInventory','readMachOLoadCommands','versionAtLeast','validatePublicMetadata','validateDeclaration','verifyPayload'].map(name=>[name,()=>{}]));
 assert.throws(()=>packageArtAdapter(options),/explicit package metadata services/);
 assert.throws(()=>packageArtAdapter({...options,version:undefined},metadata),/explicit package version/);
 assert.throws(()=>packageArtAdapter(options,metadata),/producer profiles are incompatible/);
 assert.throws(()=>packageArtAdapter({...options,commonDeclaration:{manifestRelative:'runtime.json',manifestSha256:'a'.repeat(64)}},metadata),/differs from its producer manifest/);
 assert.equal(fs.existsSync(path.join(root,'uncreated')),false,'Missing or mismatched metadata cannot create a package stage');
});
