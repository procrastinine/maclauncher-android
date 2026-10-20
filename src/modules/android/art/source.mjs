import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {exportSources} from '../native/binder-rpc/build.mjs';
import {prepareSourceTree,copyChanged,writeChanged} from '../native/upstream/source-files.mjs';
import {prepareAimRegisters} from './x18-source.mjs';
import {prepareRegisterContexts} from './register-context-source.mjs';
import {prepareElfCopies} from './elf-copy-source.mjs';
import {prepareCodePublication} from './code-publication-source.mjs';
import {prepareIslandPlacement} from './island-placement-source.mjs';
import {prepareAshmemAccess} from './ashmem-access-source.mjs';
import {prepareRelro} from './relro-source.mjs';
import {prepareAimVideo} from './video-source.mjs';
import {prepareFaultDiagnostics} from './fault-diagnostic-source.mjs';
import {prepareAimGlesInterop} from './gpu-source.mjs';
import {prepareWindowBuffers} from './window-source.mjs';
import {prepareImportedStorage} from './storage-source.mjs';
import {preparePackagePaths} from './package-paths-source.mjs';
import {prepareSignalUserMemory} from './signal-memory-source.mjs';
import {prepareTranslationAnalysis} from './translation-source.mjs';
import {prepareInitialStackLayout} from './stack-layout-source.mjs';
import {prepareProcAuxv} from './proc-auxv-source.mjs';
import {prepareVdsoVersions} from './vdso-source.mjs';
import {prepareMulticastOptions,prepareSocketPendingBytes} from './network-source.mjs';
import {prepareA64Execution} from './a64-execution-source.mjs';
import {prepareOpenedOrigins} from './opened-origin-source.mjs';
import {prepareHardwareCapabilities} from './hwcap-source.mjs';
const root=import.meta.dirname;
export const supplier=JSON.parse(fs.readFileSync(path.join(root,'supplier.json'),'utf8'));

/** Read pinned sources; adapt only an owned projection. Cargo retains the
 * upstream workspace and dependency graph, including its original lockfile. */
export function prepareAimSource({source,output,klepton}){
 prepareSourceTree(output,temporary=>{
  exportSources(source,supplier.sources.aim.commit,['.'],temporary);
  for(const name of ['aim-hostcall.patch','aim-platform.patch','aim-opened-path.patch','aim-display-modes.patch','aim-identities.patch','aim-process-liveness.patch','aim-create-mode.patch'])
   execFileSync('git',['apply','--unsafe-paths',path.join(root,'adapters',name)],{cwd:temporary,stdio:'inherit',env:{...process.env,GIT_CEILING_DIRECTORIES:path.dirname(temporary)}});
  copyChanged(path.join(root,'native/opened-path.rs'),path.join(temporary,'crates/aim-linux-abi/src/sys/opened-path.rs'));
  prepareOpenedOrigins(temporary);
  prepareHardwareCapabilities(temporary);
  copyChanged(path.join(root,'native/process-identity.rs'),path.join(temporary,'crates/aim-linux-abi/src/process-identity.rs'));
  prepareAimRegisters({aim:temporary,klepton});
  prepareRegisterContexts(temporary);
  prepareTranslationAnalysis(temporary);
  prepareInitialStackLayout(temporary);
  prepareProcAuxv(temporary);
  prepareVdsoVersions(temporary);
  prepareElfCopies(temporary);
  prepareAshmemAccess(temporary);
  prepareCodePublication(temporary);
  prepareIslandPlacement(temporary);
  prepareRelro(temporary);
  prepareAimVideo(temporary);
  prepareSignalUserMemory(temporary);
  prepareMulticastOptions(temporary);
  prepareSocketPendingBytes(temporary);
  prepareFaultDiagnostics(temporary);
  prepareAimGlesInterop(temporary);
  prepareWindowBuffers(temporary);
  prepareImportedStorage(temporary);
  preparePackagePaths(temporary);
  prepareA64Execution(temporary);
 });
 return output;
}

/** Consume the complete owned AIM projection from prepareAimSource, including
 * storage inode adapters shared by the host bootstrap and Linux ABI. */
export function prepareHostSource({aim,output}){
 const previousLock=path.join(output,"Cargo.lock"),previousSource=path.join(output,".upstream-lock");
 const lock=fs.readFileSync(path.join(aim,"Cargo.lock"));
 const preserve=fs.existsSync(previousSource)&&fs.readFileSync(previousSource).equals(lock)&&fs.existsSync(previousLock);
 const resolved=preserve?fs.readFileSync(previousLock):lock;
 prepareSourceTree(output,temporary=>{
  for(const name of ['bootstrap.rs','compositor-host.rs'])copyChanged(path.join(root,'native',name),path.join(temporary,'src',name));
  // Source modules are upstream property/classpath implementations, not copies
  // maintained in the adapter. Paths are relative to this build workspace.
  for(const name of ['bootstrap.rs','compositor-host.rs']){
   const file=path.join(temporary,'src',name);let text=fs.readFileSync(file,'utf8');
   text=text.replaceAll('../../upstream/',path.relative(path.join(output,'src'),aim).split(path.sep).join('/')+'/');writeChanged(file,text);
  }
  const dependencies=['aim-android-init','aim-host-display','aim-hostcall','aim-sync-file','aim-binder-host','aim-storage'];
  const cargo=['[package]','name = "maclauncher-art-host"','version = "0.1.0"','edition = "2024"','publish = false','[workspace]','[dependencies]','libc = "0.2"'];
  for(const name of dependencies)cargo.push(name+' = { path = '+JSON.stringify(path.relative(output,path.join(aim,'crates',name)).split(path.sep).join('/'))+' }');
  for(const [name,file]of [['android-art-properties','bootstrap.rs'],['android-art-presenter','compositor-host.rs']])cargo.push('[[bin]]','name = '+JSON.stringify(name),'path = '+JSON.stringify('src/'+file));
  cargo.push('[profile.release]','strip = false');
  writeChanged(path.join(temporary,'Cargo.toml'),cargo.join('\n')+'\n');
  // Let Cargo preserve upstream dependency versions instead of resolving a new
  // dependency universe for this small physical transport crate.
  writeChanged(path.join(temporary,'Cargo.lock'),resolved);
  writeChanged(path.join(temporary,'.upstream-lock'),lock);
 });
 return output;
}
