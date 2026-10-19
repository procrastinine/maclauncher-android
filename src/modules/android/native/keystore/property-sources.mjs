import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {createHash} from 'node:crypto';
import {writeChanged} from '../upstream/source-files.mjs';
/** Original libbase property APIs forwarded to the shared process property owner. */
export function preparePropertyHeaders({output:out,libbase,bionic}) {
fs.mkdirSync(path.join(out,'include/android-base'),{recursive:true});fs.mkdirSync(path.join(out,'include/sys'),{recursive:true});
const sha=b=>createHash('sha256').update(b).digest('hex'),adaptations=[];
function write(original,destination,transform){const before=fs.readFileSync(original,'utf8'),after=transform(before);writeChanged(destination,after);adaptations.push({original,destination,originalSha256:sha(before),adaptedSha256:sha(after)});}
const fullPropertyPath=text=>text.replaceAll('#if defined(__BIONIC__)','#if 1 /* Existing guest property owner on Darwin */').replaceAll('#if !defined(__BIONIC__)','#if 0 /* Do not build an alternate host property dictionary */');
write(path.join(libbase,'properties.cpp'),path.join(out,'properties.cpp'),fullPropertyPath);
write(path.join(libbase,'include/android-base/properties.h'),path.join(out,'include/android-base/properties.h'),fullPropertyPath);

write(path.join(bionic,'libc/include/sys/system_properties.h'),path.join(out,'include/sys/system_properties.h'),text=>text.replace('#include <sys/cdefs.h>','#include <sys/cdefs.h>\n#ifndef __BIONIC_AVAILABILITY_GUARD\n#define __BIONIC_AVAILABILITY_GUARD(api) 1\n#endif\n#ifndef __INTRODUCED_IN\n#define __INTRODUCED_IN(api)\n#endif'));
return {source:path.join(out,'properties.cpp'),include:path.join(out,'include'),adaptations};
}
