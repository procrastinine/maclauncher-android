import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import {spawnSync} from 'node:child_process';
import {prepareGlesInteropHeaders} from '../src/modules/android/art/gpu-source.mjs';

test('ART projection accepts a verified caller header checkout outside its source owner',t=>{
 const supplied=process.env.ANDROID_GLES_HEADERS;
 if(!supplied)return t.skip('Requires the existing pinned GLES header checkout');
 const temporary=fs.mkdtempSync(path.join(os.tmpdir(),'android-art-source-input-'));
 t.after(()=>fs.rmSync(temporary,{recursive:true,force:true}));
 const owner=fs.realpathSync(supplied),alias=path.join(temporary,'separate-checkout'),output=path.join(temporary,'projection');
 fs.symlinkSync(owner,alias,'dir');
 const pin=JSON.parse(fs.readFileSync(new URL('../src/modules/android/native/gles-headers.json',import.meta.url)));
 const selected=['EGL','KHR'].flatMap(family=>fs.readdirSync(path.join(owner,'include',family)).filter(name=>name.endsWith('.h')).map(name=>({path:'include/'+family+'/'+name})));
 const before=selected.map(row=>fs.statSync(path.join(owner,row.path),{bigint:true}).mtimeNs);
 const result=prepareGlesInteropHeaders({glesHeaders:alias,output});
 assert.equal(result.root,owner);assert.equal(result.manifest.revision,pin.revision);
 for(const row of selected)assert.deepEqual(fs.readFileSync(path.join(output,row.path)),fs.readFileSync(path.join(owner,row.path)));
 assert.deepEqual(selected.map(row=>fs.statSync(path.join(owner,row.path),{bigint:true}).mtimeNs),before);
 assert.equal(fs.existsSync(path.join(output,'include/GLES')),false,'Only the consumed EGL/KHR family is projected');
 assert.ok(fs.existsSync(path.join(output,'include/EGL/eglext_angle.h')),'Original transitive ANGLE extension header remains present');
 const compiled=spawnSync(process.env.CLANG||'clang',['-fsyntax-only','-x','c','-I',path.join(output,'include'),'-'],
  {encoding:'utf8',input:'#define EGL_EGLEXT_PROTOTYPES 1\n#include <EGL/egl.h>\n#include <EGL/eglext.h>\nint main(void){EGLDisplay display=EGL_NO_DISPLAY;return display!=EGL_NO_DISPLAY;}\n'});
 assert.ifError(compiled.error);assert.equal(compiled.status,0,compiled.stdout+compiled.stderr);
 const bad=path.join(temporary,'bad-input'),rejected=path.join(temporary,'rejected');
 fs.mkdirSync(path.join(bad,'include/EGL'),{recursive:true});fs.writeFileSync(path.join(bad,'include/EGL/egl.h'),'changed header');
 assert.throws(()=>prepareGlesInteropHeaders({glesHeaders:bad,output:rejected}),/does not match its recorded source/);
 assert.equal(fs.existsSync(rejected),false,'Integrity failure cannot publish partial headers');
});
