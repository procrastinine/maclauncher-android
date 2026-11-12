import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import {execFileSync} from 'node:child_process';
import {checkSources,moduleImports} from '../tools/check-sources.mjs';

test('source metadata checks exact pins, transitive imports and own Git repository',t=>{
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-source-check-'));
 t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const write=(name,data)=>{const file=path.join(root,name);fs.mkdirSync(path.dirname(file),{recursive:true});fs.writeFileSync(file,data);};
 const revision='a'.repeat(40),project={id:'source',path:'src/external/source',repository:'https://example.invalid/source',revision,pinRefs:[{kind:'json',path:'src/pins.json',property:'revision'}]};
 write('upstreams.json',JSON.stringify({schemaVersion:1,projects:[project]}));
 write('.gitmodules',`[submodule "source"]\npath = src/external/source\nurl = ${project.repository}\nmaclauncher-revision = ${revision}\nmaclauncher-role = source\n`);
 write('src/pins.json',JSON.stringify({revision}));write('src/entry.mjs',"import './nested/helper.mjs';\n");write('src/nested/helper.mjs',"import fs from 'node:fs';\n");
 assert.equal(checkSources(root,{metadataOnly:true}).sourceModules,2);
 assert.throws(()=>checkSources(root),/Initialize this source repository/);
 write('src/pins.json',JSON.stringify({revision:'b'.repeat(40)}));assert.throws(()=>checkSources(root,{metadataOnly:true}),/Source pin differs/);
 write('src/pins.json',JSON.stringify({revision}));write('src/nested/helper.mjs',"import './missing.mjs';\n");
 assert.throws(()=>checkSources(root,{metadataOnly:true}),/Missing source import: src\/nested\/helper.mjs/);
});

test('source import scanning ignores generated code and nested source templates',()=>{
 const source="import fs from 'node:fs';const generated=`Rust from 'not-an-import' ${[1].map(value=>`nested ${value}`).join('')}`;const matcher=/from ['\"]/;const helper=require('./helper');export {other} from './other.mjs';";
 assert.deepEqual(moduleImports(source),['node:fs','./helper','./other.mjs']);
});


test('every Git module is a declared upstream supplier',t=>{
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-undeclared-module-check-'));
 t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 fs.mkdirSync(path.join(root,'src'));
 fs.writeFileSync(path.join(root,'upstreams.json'),JSON.stringify({schemaVersion:1,projects:[]}));
 fs.writeFileSync(path.join(root,'.gitmodules'),'[submodule "shared"]\npath = sources/shared\nurl = https://example.invalid/shared.git\nmaclauncher-role = shared-source\n');
 assert.throws(()=>checkSources(root,{metadataOnly:true}),/Undeclared Git module: shared/);
});

test('tracked files never read through a pinned checkout',()=>{
 const root=path.resolve(import.meta.dirname,'..'),git=(...args)=>execFileSync('git',['-C',root,...args],{encoding:'utf8',maxBuffer:1<<28});
 const index=git('ls-files','-s','-z').split('\0').filter(Boolean).map(row=>{const [header,file]=row.split('\t'),[mode,object]=header.split(' ');return {mode,object,file};});
 const gitlinks=index.filter(entry=>entry.mode==='160000').map(entry=>entry.file);
 for(const link of index.filter(entry=>entry.mode==='120000')){
  const target=path.posix.normalize(path.posix.join(path.posix.dirname(link.file),git('cat-file','blob',link.object)));
  assert.ok(!target.startsWith('../'),`${link.file} points outside the repository`);
  assert.equal(gitlinks.find(gitlink=>target===gitlink||target.startsWith(gitlink+'/')),undefined,`${link.file} reads ${target} through a pinned checkout`);
 }
});

test('a supplier no build checks out is not a Git module',t=>{
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'android-reference-module-check-'));
 t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const revision='c'.repeat(40),project={id:'reference',path:'src/external/reference',repository:'https://example.invalid/reference',revision,pinRefs:[{kind:'json',path:'src/adaptation.json',property:'revision'}],referenceOnly:true};
 fs.mkdirSync(path.join(root,'src'));
 fs.writeFileSync(path.join(root,'src/adaptation.json'),JSON.stringify({repository:project.repository,revision}));
 fs.writeFileSync(path.join(root,'upstreams.json'),JSON.stringify({schemaVersion:1,projects:[project]}));
 fs.writeFileSync(path.join(root,'.gitmodules'),`[submodule "reference"]\npath = ${project.path}\nurl = ${project.repository}\nmaclauncher-revision = ${revision}\nmaclauncher-role = reference\nmaclauncher-init = false\n`);
 assert.throws(()=>checkSources(root,{metadataOnly:true}),/Reference-only Git module: reference/);
});
