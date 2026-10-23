import fs from 'node:fs';
import path from 'node:path';

/** Add the physical JIT-view check only to a test-owned supplier projection. */
export function installPhysicalJitViewProbe(aim) {
  const file = path.join(aim, 'crates/aim-linux-abi/src/sys/jit-user-copy.rs');
  const source = fs.readFileSync(file, 'utf8');
  const probe = fs.readFileSync(new URL('./physical-jit-view.rs', import.meta.url), 'utf8');
  if (source.includes(probe)) return file;
  if (source.includes('mod physical_binding_tests'))
    throw Error('The physical JIT-view test is already defined by another source.');
  fs.writeFileSync(file, source + probe);
  return file;
}
