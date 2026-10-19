import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {nativeRustPathEnvironment} from '../../native/upstream/native-paths.mjs';

/** Original GLES driver paired with the original GPU hostcall implementation.
 * The image supplies libnativewindow; the NDK supplies compilation/linking. */
export function buildArtGraphics({aim,output,cargo='cargo',env=process.env,linker,libraryDirectories=[]}){
 fs.mkdirSync(output,{recursive:true});
 const remaps=nativeRustPathEnvironment({source:import.meta.dirname,build:output,roots:[[aim,'/src/aim'],...(env.CARGO_HOME?[[env.CARGO_HOME,'/src/cargo']]:[])]},env);
 const target=path.resolve(output,'cargo');
 execFileSync(cargo,['build','--offline','--release','--target','aarch64-linux-android','--manifest-path',path.join(path.resolve(aim),'Cargo.toml'),'--package','gles','--target-dir',target,'-j','2'],
  {cwd:aim,env:{...env,...remaps,...(linker?{CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER:linker}:{}),CARGO_ENCODED_RUSTFLAGS:[remaps.CARGO_ENCODED_RUSTFLAGS,...libraryDirectories.map(directory=>'-Lnative='+path.resolve(directory))].join('\x1f')},stdio:'inherit'});
 return {glesLibrary:path.join(target,'aarch64-linux-android/release/libgles.so')};
}
