import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';

// The real callback adapter and original sync-file writer execute on CPU
// doubles. No Metal device, queue, command buffer or GPU work is created.
test('failed Metal completion retires its exact native fence with the original error/FD ABI', t=>{
 const source=process.env.ANDROID_AIM_SOURCE;
 if(!source||!process.env.ANDROID_RUST_LIBC_SOURCE)return t.skip('Requires the pinned AIM source and cached Rust dependencies');
 const output=fs.mkdtempSync(path.join(os.tmpdir(),'android-fence-completion-'));
 t.after(()=>fs.rmSync(output,{recursive:true,force:true}));
 const file=relative=>fileURLToPath(new URL(relative,import.meta.url));
 const result=spawnSync(process.execPath,[file('./helpers/android-fence-completion.mjs'),source,
  file('../src/modules/android/art/fence-source.mjs'),file('../src/modules/android/native/angle-fence-completion.h'),
  file('../src/modules/android/art/native/metal-fence-completion.rs'),output],{encoding:'utf8'});
 assert.ifError(result.error);
 assert.equal(result.status,0,result.stdout+result.stderr);
 assert.match(result.stdout,/PASS copied completion lifetime/);
 assert.match(result.stdout,/test result: ok\. 1 passed/);
});

test('guest native-fence waits report negative status without consuming borrowed descriptors', t=>{
 const source=process.env.ANDROID_AIM_SOURCE;
 if(!source||!process.env.ANDROID_RUST_LIBC_SOURCE)return t.skip('Requires the pinned AIM source and cached Rust dependencies');
 const output=fs.mkdtempSync(path.join(os.tmpdir(),'android-fence-wait-'));
 t.after(()=>fs.rmSync(output,{recursive:true,force:true}));
 const file=relative=>fileURLToPath(new URL(relative,import.meta.url));
 const result=spawnSync(process.execPath,[file('./helpers/android-fence-wait.mjs'),source,
  file('../src/modules/android/art/fence-source.mjs'),file('../src/modules/android/art/native/guest-fence-wait.rs'),output],{encoding:'utf8'});
 assert.ifError(result.error);
 assert.equal(result.status,0,result.stdout+result.stderr);
 const receipt=JSON.parse(fs.readFileSync(path.join(output,'result.json'),'utf8'));
 assert.equal(receipt.results[0].status,1,'Original readiness-only waits hide the fence error');
 assert.equal(receipt.results[1].status,0,'Original sync-file status reaches the EGL wait boundary');
});
