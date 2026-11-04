import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import os from 'node:os';
import {execFileSync} from 'node:child_process';
import {nativeRustPathEnvironment,publishNativeBinary} from '../native/upstream/native-paths.mjs';
import {prepareSourceTree,writeChanged} from '../native/upstream/source-files.mjs';

/** Original AIM image reader, delivered with ART so image acquisition does not
 * install another execution runtime or need a compiler on the user's Mac. */
export function buildImageExtractor({aim,output,cargo='cargo',minimumMacOS='13.0',env=process.env}) {
  aim=path.resolve(aim); output=path.resolve(output);
  const source=path.join(output,'source'),target=path.join(output,'target');
  prepareSourceTree(source,temporary=>{
    fs.cpSync(path.join(aim,'tools/android-image-extract/src'),path.join(temporary,'src'),{recursive:true});
    const original=fs.readFileSync(path.join(aim,'tools/android-image-extract/Cargo.toml'),'utf8');
    const manifest=original.replace(/^.*\.workspace\s*=\s*true.*$/gm,line=>line.startsWith('sha2.')?'sha2 = "0.10"':'');
    if(manifest===original||manifest.includes('.workspace')) throw Error('The Android image reader manifest changed.');
    writeChanged(path.join(temporary,'Cargo.toml'),manifest);
    // Retain the original resolved dependency versions without building the
    // rest of the AIM workspace. Cargo prunes unrelated lockfile entries.
    fs.copyFileSync(path.join(aim,'Cargo.lock'),path.join(temporary,'Cargo.lock'));
  });
  execFileSync(cargo,['build','--offline','--release','--manifest-path',path.join(source,'Cargo.toml'),'--target-dir',target,'-j','2'],{
    cwd:source,stdio:'inherit',env:{...env,MACOSX_DEPLOYMENT_TARGET:minimumMacOS,
      ...nativeRustPathEnvironment({source,build:output,roots:[[aim,'/src/aim'],[env.CARGO_HOME||path.join(os.homedir(),'.cargo'),'/src/cargo']]},env)}
  });
  const executable=path.join(output,'android-image-extract');
  publishNativeBinary(path.join(target,'release/android-image-extract'),executable);
  return {executable,sha256:crypto.createHash('sha256').update(fs.readFileSync(executable)).digest('hex'),minimumMacOS};
}
