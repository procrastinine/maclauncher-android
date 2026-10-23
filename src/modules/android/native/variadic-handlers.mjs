import fs from 'node:fs';
import path from 'node:path';

// The existing AAPCS64 thunks declare their service handler and fixed arity.
// Other guest ABIs reuse those exact handlers after constructing the same
// AAPCS64 va_list, rather than owning another printf/logging/file service.
export function prepareVariadicHandlers({klepton,build,wideStdio,additionalThunks=[]}){
  // JNI slots belong to the actual VM facade's descriptor-aware dispatcher.
  // Keep the native libc thunks, without the standalone synthetic JNI table's
  // private entry points and handlers from the excluded kl_jni.c universe.
  let original=fs.readFileSync(path.join(klepton,'runtime/libc/kl_va_thunks.S'),'utf8');
  const begin=original.indexOf('// ---- JNI\'s plain varargs Call*Method family ----');
  const end=original.indexOf('// ---- long double returns ----',begin);
  if(begin<0||end<begin)throw Error('Pinned standalone JNI variadic boundary changed');
  original=original.slice(0,begin)+original.slice(end);
  const thunks=path.join(build,'native-libc-thunks.S');fs.writeFileSync(thunks,original);
  const files=[thunks,path.join(import.meta.dirname,'native-varargs.S'),path.join(wideStdio.output,'thunks.S'),...additionalThunks];
  const entries=new Map();
  for(const file of files)for(const match of fs.readFileSync(file,'utf8').matchAll(/^KL_VTHUNK\s+(\w+),\s*(\w+),\s*(\d+)\s*$/gm))entries.set(match[1],{name:match[1],handler:match[2],fixed:Number(match[3])});
  const source=path.join(build,'variadic-handlers.c');
  fs.writeFileSync(source,'#include <stddef.h>\n#include '+JSON.stringify(path.join(import.meta.dirname,'variadic-handlers.h'))+'\n'+[...entries.values()].map(e=>`extern void ${e.name}(void),${e.handler}(void);`).join('\n')+'\nconst KLVariadicHandler *klj_variadic_handler(const void *entry){\nstatic const struct {const void *entry;KLVariadicHandler service;} entries[]={\n'+[...entries.values()].map(e=>`{(void *)${e.name},{(void *)${e.handler},${e.fixed}}},`).join('\n')+'\n};\nfor(size_t n=0;n<sizeof(entries)/sizeof(entries[0]);n++)if(entries[n].entry==entry)return &entries[n].service;\nreturn NULL;\n}\n');
  return {source,thunks};
}
