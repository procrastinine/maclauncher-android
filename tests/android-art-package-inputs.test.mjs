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

test('ART packaging inventories flat licence texts as ordinary payload files',async t=>{
 const {addLicense}=await import('../src/modules/android/native/upstream/licenses.mjs');
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-package-licenses-'));t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const write=(name,text)=>{const file=path.join(root,name);fs.mkdirSync(path.dirname(file),{recursive:true});fs.writeFileSync(file,text);return file;};
 const imageSha256='a'.repeat(64),supplierRoot=path.join(root,'supplier'),commonRoot=path.join(root,'common');
 write('supplier/runtime.json',JSON.stringify({schemaVersion:1,execution:'art',components:{image:'image'},imageAcquisition:{delivery:'explicit-download'},android:{imageSha256}}));
 write('common/runtime.json',JSON.stringify({profile:{javaMajor:21}}));write('common/native/SDL3.framework/SDL3','library');
 const text=write('LICENSE','Same licence text\n');addLicense(supplierRoot,'ART',text,'MIT');addLicense(commonRoot,'JVM',text,'MIT');
 const extractor=write('extractor','extractor'),sourceArchive=write('source.tar.gz','source'),output=path.join(root,'output');
 const {createHash}=await import('node:crypto');
 const metadata={fileInventory:directory=>{const walk=(at,prefix='')=>fs.readdirSync(at,{withFileTypes:true}).flatMap(entry=>entry.isDirectory()?walk(path.join(at,entry.name),prefix+entry.name+'/'):[prefix+entry.name]);return walk(directory);},
  readMachOLoadCommands:()=>undefined,versionAtLeast:()=>false,validatePublicMetadata:()=>{},validateDeclaration:()=>{},verifyPayload:(directory,declaration)=>{for(const row of declaration.files)assert.equal(createHash('sha256').update(fs.readFileSync(path.join(directory,row.path))).digest('hex'),row.sha256);}};
 const result=packageArtAdapter({supplierRoot,commonRoot,extractor,sourceArchive,output,version:'0.2.1',originalImage:{archiveSha256:imageSha256,content:{sha256:'b'.repeat(64),records:1,bytes:1}}},metadata);
 assert.equal(result.root,output);assert.equal(Object.hasOwn(result.declaration,'notices'),false);
 assert.deepEqual(fs.readdirSync(path.join(output,'licenses')).sort(),['MIT.txt','components.txt']);
 assert.equal(result.declaration.files.filter(row=>row.path.startsWith('licenses/')).length,2);
 assert.equal(fs.readFileSync(path.join(output,'licenses/components.txt'),'utf8'),'ART: MIT.txt\nJVM: MIT.txt\n');
 assert.equal(fs.existsSync(path.join(output,'art/licenses')),false);
});
