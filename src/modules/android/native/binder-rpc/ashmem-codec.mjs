import {addLicense} from '../upstream/licenses.mjs';
import fs from 'node:fs';import path from 'node:path';import {execFileSync} from 'node:child_process';
import {exportSources} from './build.mjs';import {prepareSourceTree,copyChanged,writeChanged} from '../upstream/source-files.mjs';import {nativeRustPathEnvironment} from '../upstream/native-paths.mjs';
const revision=JSON.parse(fs.readFileSync(new URL('../source-pins.json',import.meta.url))).aim;
/** Statically share AIM's exact ashmem representation with native producers.
 * No independent codec, process or runtime library is introduced. */
export function buildAshmemCodec({source,output,env=process.env,cargo='cargo',rustc}){
 const project=path.join(output,'source');
 prepareSourceTree(project,temporary=>{
  exportSources(source,revision,['crates/aim-ashmem/src/lib.rs','Cargo.lock','LICENSE','NOTICE'],path.join(temporary,'original'));
  copyChanged(path.join(import.meta.dirname,'ashmem-codec.rs'),path.join(temporary,'lib.rs'));
  copyChanged(path.join(import.meta.dirname,'ashmem-access.rs'),path.join(temporary,'ashmem-access.rs'));
  const lock=fs.readFileSync(path.join(temporary,'original/Cargo.lock'),'utf8'),version=lock.match(/name = "libc"\nversion = "([^"]+)"/)[1];
  writeChanged(path.join(temporary,'Cargo.toml'),'[package]\nname="maclauncher-ashmem-codec"\nversion="0.1.0"\nedition="2024"\n[workspace]\n[lib]\npath="lib.rs"\ncrate-type=["staticlib"]\n[dependencies]\nlibc="='+version+'"\n');
 });
 const environment={...env,...(rustc?{RUSTC:rustc}:{}),MACOSX_DEPLOYMENT_TARGET:'13.0'};
 Object.assign(environment,nativeRustPathEnvironment({source:import.meta.dirname,build:output,roots:[[source,'/src/aim'],...(env.CARGO_HOME?[[env.CARGO_HOME,'/src/cargo']]:[])]},environment));
 const target=path.join(output,'cargo');
 execFileSync(cargo,['build','--offline','--release','--manifest-path',path.join(project,'Cargo.toml'),'--target-dir',target],{env:environment,stdio:'inherit'});
 const licenses=path.join(output,'licenses');
 for(const name of ['LICENSE','NOTICE'])addLicense(path.dirname(licenses),'ashmem-codec',path.join(project,'original',name));
 return path.join(target,'release/libmaclauncher_ashmem_codec.a');
}
