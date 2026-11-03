import {addLicense} from '../upstream/licenses.mjs';
import {prepareSourceTree} from '../upstream/source-files.mjs';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import {execFileSync} from 'node:child_process';
import {exportSources} from '../binder-rpc/build.mjs';
import {nativeRustPathEnvironment} from '../upstream/native-paths.mjs';
import {prepareExecutableMemoryApi} from './source.mjs';

/** Reuse the original AIM JIT allocator, instruction-store and fault-resume
 * code. This target builds only the native memory component; no Android image
 * or services are started or built. Cargo retains its normal incremental cache. */
export function buildExecutableMemory({source,revision,output,rustToolchain,env=process.env}){
 const project=path.join(output,'source');
 prepareSourceTree(project,temporary=>{
  exportSources(source,revision,['.'],temporary);
  prepareExecutableMemoryApi(temporary);
 });
 const environment={...env,PATH:path.join(rustToolchain,'bin')+path.delimiter+env.PATH,
  RUSTC:path.join(rustToolchain,'bin/rustc'),MACOSX_DEPLOYMENT_TARGET:'13.0'};
 Object.assign(environment,nativeRustPathEnvironment({source:import.meta.dirname,build:output,
  roots:[[source,'/src/aim'],[env.CARGO_HOME||path.join(os.homedir(),'.cargo'),'/src/cargo'],[rustToolchain,'/toolchain/rust']]},environment));
 const target=path.join(output,'cargo');
 execFileSync(path.join(rustToolchain,'bin/cargo'),['rustc','--locked','--release','-j','2',
  '--manifest-path',path.join(project,'Cargo.toml'),'--target-dir',target,'-p','aim-linux-abi','--lib','--crate-type','staticlib'],
  {env:environment,stdio:'inherit'});
 // The original component links into the executor's existing native owner.
 // No additional runtime image, symbol loader or install name is introduced.
 const archive=path.join(target,'release/libaim_linux_abi.a');
 const licenses=path.join(output,'licenses');
 for(const name of ['LICENSE','NOTICE','LICENSING.md','THIRD_PARTY_NOTICES.md'])
  if(fs.existsSync(path.join(project,name)))addLicense(path.dirname(licenses),'executable-memory',path.join(project,name));
 return {archive,linkFlags:['-framework','IOKit','-lsandbox'],licenses};
}
