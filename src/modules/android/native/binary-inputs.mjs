import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';

export function verifyBinaryInputs(directory, entries, pathKey = 'path') {
  const root = fs.realpathSync(directory);
  return entries.map(entry => {
    const name = entry[pathKey];
    if (typeof name !== 'string' || path.basename(name) !== name || name === '.' || name === '..') throw Error('Invalid binary input name');
    const file = path.join(root, name), resolved = fs.realpathSync(file);
    if (path.dirname(resolved) !== root || !fs.statSync(resolved).isFile()) throw Error('Binary input leaves its directory: ' + name);
    const bytes = fs.readFileSync(resolved);
    const sha256 = crypto.createHash('sha256').update(bytes).digest('hex');
    if (bytes.length !== entry.size || sha256 !== entry.sha256) throw Error('Binary input does not match its recorded release: ' + name);
    return {name, file: resolved, size: bytes.length, sha256};
  });
}
