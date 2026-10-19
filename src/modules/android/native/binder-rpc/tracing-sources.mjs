import fs from 'node:fs';
import path from 'node:path';

/** The original public NDK module shares Binder's original libcutils host sink.
 * Its whole declared namespace enters the shared idempotent ELF resolver registry. */
export function prepareNativeTrace({frameworkSource,nativeSource,pins,source,output,sources,flags,exportSources}){
  const framework=path.join(source,'framework'),native=path.join(source,'native');
  exportSources(frameworkSource,pins.aospFramework,['native/android/trace.cpp'],framework);
  exportSources(nativeSource,pins.aospNative,['include/android/trace.h'],native);
  const header=fs.readFileSync(path.join(native,'include/android/trace.h'),'utf8');
  const names=[...header.replace(/\/\*[\s\S]*?\*\//g,'').matchAll(/^(?:bool|void)\s+(ATrace_\w+)\s*\(/gm)].map(match=>match[1]);
  const generated=path.join(output,'trace'),include=path.join(generated,'include');
  fs.mkdirSync(path.join(include,'android'),{recursive:true});
  fs.writeFileSync(path.join(include,'android/trace.h'),header.replace(/__INTRODUCED_IN\(\d+\)/g,''));
  const bridge=path.join(generated,'imports.cpp');
  fs.writeFileSync(bridge,`#include <android/trace.h>
#include <cstring>
#include ${JSON.stringify(path.resolve(import.meta.dirname,'../native-import-registry.h'))}
static void* resolveTrace(const char* name){
${names.map(name=>`    if(!strcmp(name,"${name}"))return reinterpret_cast<void*>(&${name});`).join('\n')}
    return nullptr;
}
bool kljInstallTraceImports(){
    return klj_register_import_resolver(resolveTrace)!=0;
}
`);
  sources.push(path.join(framework,'native/android/trace.cpp'),bridge);
  flags.push('-I'+include);
  return {names,include,bridge};
}

/** The complete original tracing namespace owns these former sample exports. */
export function retireTraceSamples(source){
  return source.replace(/^static void klm_ATrace_\w+\([^\n]*\}\n/gm,'')
    .replace(/^\s*M\("ATrace_\w+",\s*klm_ATrace_\w+\),\n/gm,'');
}

