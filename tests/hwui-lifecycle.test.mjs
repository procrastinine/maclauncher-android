import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {adaptCommonPool,prepareCommonPool} from '../src/modules/android/native/hwui/common-pool-source.mjs';
const fixture=new URL('./fixtures/hwui-lifecycle/',import.meta.url);
test('original CommonPool workers finish before member destruction, including failed construction and worker finalization',()=>{
 const work=fs.mkdtempSync(path.join(os.tmpdir(),'android-hwui-lifetime-'));
 try {
 const adapted=adaptCommonPool({header:fs.readFileSync(new URL('original/thread/CommonPool.h',fixture),'utf8'),source:fs.readFileSync(new URL('original/thread/CommonPool.cpp',fixture),'utf8')});
 fs.mkdirSync(path.join(work,'adapted/thread'),{recursive:true});fs.writeFileSync(path.join(work,'adapted/thread/CommonPool.h'),adapted.header);fs.writeFileSync(path.join(work,'adapted/thread/CommonPool.cpp'),adapted.source);
 const run=(bin,args)=>{const r=spawnSync(bin,args,{encoding:'utf8',timeout:15000});assert.ifError(r.error);return r;};
 const layout={};
 for(const kind of ['original','adapted']) {
 const source=kind==='original'?new URL('original/',fixture).pathname:path.join(work,'adapted');
 const binary=path.join(work,kind+"-test");const args=['-std=c++20','-O1','-g','-pthread','-include','atomic','-include','array','-include','cstdio','-include','thread','-include','unistd.h','-I'+source,'-I'+new URL('support/',fixture).pathname,'-I'+new URL('platform/',fixture).pathname,...(kind==='adapted'?['-DCOMMON_POOL_TESTING']:[]),new URL('main.cpp',fixture).pathname,'-o',binary];
 const compiled=run('clang++',args);assert.equal(compiled.status,0,compiled.stderr);layout[kind]=JSON.parse(run(binary,['layout']).stdout);
 const blocked=run(binary,['blocked']);assert.equal(blocked.status,kind==='original'?42:0,blocked.stderr);
 if(kind==='adapted')for(const mode of ['constructor-failure','worker-finalization','worker-exit']){const r=run(binary,[mode]);assert.equal(r.status,mode==='worker-exit'?23:0,r.stderr);}
 }
 assert.deepEqual(layout.original,layout.adapted);
 } finally {fs.rmSync(work,{recursive:true,force:true});}
});

test('HWUI adaptation copies a symlinked original source and rejects aliased output',()=>{
 const work=fs.mkdtempSync(path.join(os.tmpdir(),'android-hwui-projection-'));
 try {
 const original=path.join(work,'original');fs.cpSync(new URL('original/',fixture),original,{recursive:true});const framework=path.join(work,'framework');fs.mkdirSync(path.join(framework,'libs'),{recursive:true});fs.symlinkSync(original,path.join(framework,'libs/hwui'));
 const before=fs.readFileSync(path.join(original,'thread/CommonPool.cpp'));const output=path.join(work,'adapted');prepareCommonPool({framework,output});
 assert.equal(fs.lstatSync(output).isSymbolicLink(),false);assert.deepEqual(fs.readFileSync(path.join(original,'thread/CommonPool.cpp')),before);assert.notDeepEqual(fs.readFileSync(path.join(output,'thread/CommonPool.cpp')),before);
 const alias=path.join(work,'bad-output');fs.symlinkSync(original,alias);assert.throws(()=>prepareCommonPool({framework,output:alias}),/owned directory/);assert.deepEqual(fs.readFileSync(path.join(original,'thread/CommonPool.cpp')),before);
 } finally {fs.rmSync(work,{recursive:true,force:true});}
});
