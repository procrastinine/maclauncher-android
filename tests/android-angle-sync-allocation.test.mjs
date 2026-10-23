import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';

// Compile the actual upstream/adapted methods with an Objective-C queue whose
// commandBuffer returns nil. Foundation supplies the real nil-message behavior;
// this control does not create a Metal device, queue or GPU submission.
test('Metal sync allocation rejects an unbacked command buffer before publishing a fence', t => {
  const source = process.env.ANDROID_ANGLE_SOURCE_ROOT;
  if (!source) return t.skip('Requires the pinned ANGLE source checkout');
  const output = fs.mkdtempSync(path.join(os.tmpdir(), 'android-angle-sync-'));
  t.after(() => fs.rmSync(output, {recursive: true, force: true}));
  const result = spawnSync(process.execPath, [
    fileURLToPath(new URL('./helpers/android-angle-sync-allocation.mjs', import.meta.url)),
    source,
    fileURLToPath(new URL('../src/modules/android/native/angle-sync-source.mjs', import.meta.url)),
    output,
  ], {encoding: 'utf8'});
  assert.ifError(result.error);
  assert.equal(result.status, 0, result.stdout + result.stderr);
  const receipt = JSON.parse(fs.readFileSync(path.join(output, 'result.json'), 'utf8'));
  assert.equal(receipt.results[0].status, 1, 'Original source exposes the invalid sync');
  assert.equal(receipt.results[1].status, 0, 'Owned adapter rejects the allocation failure');
});


test('ART native fence allocation keeps EGL_BAD_ALLOC across the original host-call boundary', t => {
  const source = process.env.ANDROID_AIM_SOURCE;
  if (!source) return t.skip('Requires the pinned AIM source checkout');
  const output = fs.mkdtempSync(path.join(os.tmpdir(), 'android-art-fence-'));
  t.after(() => fs.rmSync(output, {recursive: true, force: true}));
  const result = spawnSync(process.execPath, [
    fileURLToPath(new URL('./helpers/android-art-fence-allocation.mjs', import.meta.url)),
    source,
    fileURLToPath(new URL('../src/modules/android/art/fence-source.mjs', import.meta.url)),
    output,
  ], {encoding: 'utf8'});
  assert.ifError(result.error);
  assert.equal(result.status, 0, result.stdout + result.stderr);
  const receipt = JSON.parse(fs.readFileSync(path.join(output, 'result.json'), 'utf8'));
  assert.equal(receipt.results[0].status, 1);
  assert.equal(receipt.results[1].status, 0);
});
