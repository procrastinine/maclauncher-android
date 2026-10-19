import fs from 'node:fs';
import path from 'node:path';

// Preserve the original Android record declaration. The allocation owner stays
// Darwin's malloc subsystem; only its statistics cross this platform boundary.
export function prepareAllocator({include,output}) {
  const declaration=fs.readFileSync(path.join(include,'malloc.h'),'utf8');
  const body=declaration.match(/^#define __MALLINFO_BODY[^\n]*(?:\n[^\n]*\\)*\n[^\n]*/m)?.[0];
  if(!body)throw Error('Original Android allocator record declaration is unavailable');
  fs.mkdirSync(output,{recursive:true});
  fs.writeFileSync(path.join(output,'allocator-abi.h'),'#pragma once\n#include <stddef.h>\n'+body+'\nstruct klj_mallinfo { __MALLINFO_BODY };\n#undef __MALLINFO_BODY\n');
  return {source:path.join(import.meta.dirname,'bridge.c')};
}
