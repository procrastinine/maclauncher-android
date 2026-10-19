import fs from 'node:fs';
import path from 'node:path';
import {writeChanged} from './upstream/source-files.mjs';

// Exported build projections omit .git. Keep ANGLE's commit generator and its
// generated shader-program version in the actual driver identity, so clients
// can distinguish incompatible application-owned program binary caches.
export function prepareAngleIdentity(source, output, revision) {
  if (!/^[a-f0-9]{40}$/.test(revision)) throw Error('Invalid ANGLE source revision');
  const patch = (file, before, after) => {
    const text = fs.readFileSync(path.join(source, file), 'utf8');
    if (text.split(before).length !== 2) throw Error('ANGLE identity source changed: '+file);
    writeChanged(path.join(output, file), text.replace(before, after));
  };
  patch('src/commit_id.py',
    "commit_id = os.environ.get('ANGLE_UPSTREAM_HASH')",
    "commit_id = os.environ.get('ANGLE_UPSTREAM_HASH') or "+JSON.stringify(revision.slice(0, 12)));
  patch('src/common/angle_version.h',
    '" git hash: " ANGLE_COMMIT_HASH',
    '" git hash: " ANGLE_COMMIT_HASH " program binary: " ANGLE_PROGRAM_VERSION');
}
