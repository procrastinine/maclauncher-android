import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
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
