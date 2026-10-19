import fs from 'node:fs';
import path from 'node:path';
import {projectCodeLocationException} from './code-location-source.mjs';

/** Keep original Mach dispatch/replies; diagnose only this handler's failures. */
export function prepareExceptionHandler({source, output}) {
  const input=path.join(source,'src/dynarmic/backend/exception_handler_macos.cpp');
  let text=fs.readFileSync(input,'utf8');
  const count=(text.match(/fmt::print\(stderr, /g)||[]).length;
  if(count!==10)throw Error('Unexpected Dynarmic Mach diagnostic source');
  // A PC outside registered CPU code belongs to the next exception/signal
  // owner. KERN_FAILURE is dispatch, not an unhandled process fault. Keep the
  // original return and state copy; final owners retain terminal diagnostics.
  for(const register of ['rip','pc']){
    const report='        fmt::print(stderr, "Unhandled EXC_BAD_ACCESS at '+register+' {:#016x}\\n", ts->__'+register+');\n';
    if(text.split(report).length!==2)throw Error('Unexpected Dynarmic unowned-PC diagnostic');
    text=text.replace(report,'');
  }
  text=text.replace('#include <fmt/format.h>','#include "exception-log.h"')
    .replaceAll('fmt::print(stderr, ','ReportException(')
    .replaceAll('{:#08x} ({})','%#08x (%s)')
    .replaceAll('{:#016x}','%#016llx');
  text=projectCodeLocationException(text);
  fs.mkdirSync(path.dirname(output),{recursive:true});
  if(!fs.existsSync(output)||fs.readFileSync(output,'utf8')!==text)fs.writeFileSync(output,text);
  return output;
}
if(process.argv[1]&&path.resolve(process.argv[1])===import.meta.filename)
  prepareExceptionHandler({source:path.resolve(process.argv[2]),output:path.resolve(process.argv[3])});
