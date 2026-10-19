import fs from 'node:fs';import path from 'node:path';
export function prepareImportRegistry(shim){
 let source=fs.readFileSync(shim,'utf8');
 for(const [before,after]of [
  ['void *(*kl_shim_override)(const char *name);','#include '+JSON.stringify(path.join(import.meta.dirname,'native-import-registry.h'))],
  ['if (kl_shim_override) { void *o = kl_shim_override(name); if (o) return o; }','{ void *o = klj_resolve_import(name); if (o) return o; }']
 ]){if(source.split(before).length!==2)throw Error('Pinned native import registration changed');source=source.replace(before,after);}
 fs.writeFileSync(shim,source);
}
