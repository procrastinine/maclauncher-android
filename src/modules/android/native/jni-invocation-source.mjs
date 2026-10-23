import fs from 'node:fs';
import path from 'node:path';

/** Keep the original invocation import, backed by the real process VM owner. */
export function prepareJniInvocationSource(file){
  let source=fs.readFileSync(file,'utf8');
  const before=`    void *vm = kl_jni_vm();
    if (nVMs) *nVMs = vm ? 1 : 0;
    if (vmBuf && bufLen >= 1 && vm) vmBuf[0] = vm;
    return 0;   // JNI_OK`;
  if(source.split(before).length!==2)throw Error('Pinned JNI invocation boundary changed');
  source=source.replace(before,'    return klj_jni_get_created_vms((JavaVM **)vmBuf,(jsize)bufLen,(jsize *)nVMs);');
  source='#include '+JSON.stringify(path.join(import.meta.dirname,'jni-invocation.h'))+'\n'+source;
  source=source.replace('There is one synthetic VM here; return it.','The process loader publishes the real VM and its Android ABI facade.');
  fs.writeFileSync(file,source);
}
