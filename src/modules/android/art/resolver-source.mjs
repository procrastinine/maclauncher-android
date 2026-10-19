import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {prepareSourceTree} from '../native/upstream/source-files.mjs';
import {originalTreeFiles} from '../native/upstream/reuse.mjs';

/** Original stable AIDL client for the matching image's complete resolver.
 * The server remains the supplied APEX binary, including all newer methods. */
export function prepareResolverClient({output,compiler,sourceRoot}){
 const records=[['packages/modules/DnsResolver/aidl_api/dnsresolver_aidl_interface','15'],['packages/modules/Connectivity/staticlibs/netd/aidl_api/netd_event_listener_interface','1']].map(([name,version])=>({directory:path.join(sourceRoot,name,version),version}));
 const includes=records.flatMap(record=>['-I',record.directory]);
 prepareSourceTree(output,temporary=>{
  for(const record of records){
   const hash=fs.readFileSync(path.join(record.directory,'.hash'),'utf8').trim().split(/\s+/).at(-1);
   execFileSync(compiler,['--lang=ndk','--structured','--version='+record.version,'--hash='+hash,'--min_sdk_version=29','--omit_invocation','-a','-o',temporary,'-h',path.join(temporary,'include'),...includes,...originalTreeFiles(record.directory).filter(file=>file.endsWith('.aidl'))],{stdio:'inherit'});
  }
 });
 return {sources:originalTreeFiles(output).filter(file=>file.endsWith('.cpp')),include:path.join(output,'include'),publicInclude:path.join(sourceRoot,'packages/modules/DnsResolver/include')};
}
