import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';
import {snapshotSources,sourceInventory} from '../src/modules/android/native/build-sources.mjs';
import {relativeImports,resolveImport} from '../tools/check-sources.mjs';

test('source aliases snapshot the complete owner and separately supplied binary tools',t=>{
  const temporary=fs.mkdtempSync(path.join(os.tmpdir(),'android-source-owner-'));
  t.after(()=>fs.rmSync(temporary,{recursive:true,force:true}));
  const source=path.join(temporary,'source'),native=path.join(source,'src/modules/android/native');
  const tools=path.join(temporary,'build/binary-tools'),snapshot=path.join(temporary,'snapshot');
  fs.mkdirSync(native,{recursive:true});fs.mkdirSync(tools,{recursive:true});
  fs.writeFileSync(path.join(native,'original.c'),'original native source');
  fs.writeFileSync(path.join(native,'build.mjs'),'original producer entry');
  fs.writeFileSync(path.join(native,'../profile.js'),'original profile');
  fs.mkdirSync(path.join(native,'../art'));
  fs.writeFileSync(path.join(native,'../art/source.mjs'),'original ART adapter');
  const catalogs=['asm-binary.json','apksig-binary.json'];
  catalogs.forEach((name,index)=>{
    const bytes=Buffer.from('verified tool '+index),file={path:'tool'+index+'.jar',size:bytes.length,
      sha256:crypto.createHash('sha256').update(bytes).digest('hex')};
    fs.writeFileSync(path.join(native,name),JSON.stringify({files:[file]}));
    fs.writeFileSync(path.join(tools,file.path),bytes);
  });
  const alias=path.join(temporary,'legacy-native');fs.mkdirSync(alias);
  fs.symlinkSync(path.join(native,'build.mjs'),path.join(alias,'build.mjs'));
  fs.writeFileSync(path.join(alias,'unused-cache'),'caller-owned cache, not source');
  const before=sourceInventory(source);
  const captured=snapshotSources(alias,snapshot,{toolInputs:tools});
  assert.equal(fs.readFileSync(path.join(snapshot,'profile.js'),'utf8'),'original profile');
  assert.equal(fs.readFileSync(path.join(snapshot,'art/source.mjs'),'utf8'),'original ART adapter');
  assert.equal(captured.ownedSources.filter(file=>file.path.startsWith('tools/')).length,2);
  assert.deepEqual(sourceInventory(source),before,'snapshotting never writes the source owner');
  assert.equal(fs.existsSync(path.join(native,'tools')),false);
});

test('the ordinary source build reuses verified tools outside its read-only checkout',t=>{
  const temporary=fs.mkdtempSync(path.join(os.tmpdir(),'android-tool-owner-'));
  t.after(()=>fs.rmSync(temporary,{recursive:true,force:true}));
  const source=path.join(temporary,'source'),native=path.join(source,'src/modules/android/native');
  const tools=path.join(temporary,'cache/binary-tools'),output=path.join(temporary,'output');
  fs.mkdirSync(path.join(source,'tools'),{recursive:true});fs.mkdirSync(native,{recursive:true});fs.mkdirSync(tools,{recursive:true});fs.mkdirSync(output);
  fs.copyFileSync(new URL('../tools/build.mjs',import.meta.url),path.join(source,'tools/build.mjs'));
  for(const [index,name]of ['asm-binary.json','apksig-binary.json'].entries()){
    const bytes=Buffer.from('verified cache '+index),file={path:'tool'+index+'.jar',size:bytes.length,
      sha256:crypto.createHash('sha256').update(bytes).digest('hex')};
    fs.writeFileSync(path.join(native,name),JSON.stringify({url:'https://invalid.invalid/no-download',files:[file]}));
    fs.writeFileSync(path.join(tools,file.path),bytes);
  }
  fs.writeFileSync(path.join(native,'build.mjs'),`import fs from 'node:fs';import path from 'node:path';
    const args=process.argv.slice(2),options={};for(let i=0;i<args.length;i+=2)options[args[i].slice(2)]=args[i+1];
    fs.writeFileSync(path.join(options.output,'observed.json'),JSON.stringify(options));`);
  const inputs=path.join(temporary,'inputs.json');fs.writeFileSync(inputs,JSON.stringify({output,'build-work':path.dirname(tools)}));
  const before=sourceInventory(source);
  execFileSync(process.execPath,[path.join(source,'tools/build.mjs'),'--inputs',inputs]);
  const observed=JSON.parse(fs.readFileSync(path.join(output,'observed.json')));
  assert.equal(observed['tool-inputs'],tools);
  assert.equal(fs.existsSync(path.join(native,'tools')),false);
  assert.deepEqual(sourceInventory(source),before,'the ordinary entry point never downloads into its source checkout');
});


test('standalone runtime checks never import launcher orchestration',()=>{
  const root=path.resolve(import.meta.dirname,'..');
  const modules=[];
  function visit(directory){
    for(const entry of fs.readdirSync(directory,{withFileTypes:true})){
      const file=path.join(directory,entry.name);
      if(entry.isDirectory())visit(file);
      else if(/\.(?:cjs|mjs|js)$/.test(file))modules.push(file);
    }
  }
  visit(path.join(root,'tests'));
  for(const file of modules)for(const specifier of relativeImports(file))
    assert.ok(resolveImport(file,specifier),path.relative(root,file)+' imports '+specifier);
});
