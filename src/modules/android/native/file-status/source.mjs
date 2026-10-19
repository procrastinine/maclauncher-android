import fs from 'node:fs';
import path from 'node:path';

/** Extend the existing native stat owner; retain its single Darwin/Bionic
 * conversion and its virtual-file, namespace and identity handling. */
export function adaptFileStatusSource(source){
  return source+'\n'+fs.readFileSync(path.join(import.meta.dirname,'status.inc'),'utf8');
}
