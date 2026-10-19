import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {generateArtHalAidl} from '../hal-aidl.mjs';
import {nativeRustPathEnvironment} from '../../native/upstream/native-paths.mjs';

/** Complete original Composer3 HAL with the owned native display-mode seam.
 * Its original frozen AIDL, SurfaceFlinger transactions and buffer/fence owner
 * remain the display implementation. Cargo reuses the same supplier workspace. */
export function buildArtComposer({aim,output,cargo='cargo',env=process.env,linker,libraryDirectories=[],aidl,python,apiRoot}){
 // AIM's original bindgen recipe resolves the same frozen Binder headers
 // relative to its owned projection. Alias existing inputs; never copy AOSP.
 if(apiRoot){const input=path.join(aim,'_build/aosp'),original=path.dirname(path.resolve(apiRoot));fs.mkdirSync(path.dirname(input),{recursive:true});if(!fs.existsSync(input))fs.symlinkSync(original,input,'dir');else if(fs.realpathSync(input)!==fs.realpathSync(original))throw Error('Composer AOSP inputs differ from its frozen AIDL source');}
 generateArtHalAidl({aim,aidl,python,apiRoot,hal:'graphics/composer'});
 fs.mkdirSync(output,{recursive:true});
 const remaps=nativeRustPathEnvironment({source:import.meta.dirname,build:output,roots:[[aim,'/src/aim'],...(env.CARGO_HOME?[[env.CARGO_HOME,'/src/cargo']]:[])]},env);
 const target=path.resolve(output,'cargo');
 execFileSync(cargo,['build','--offline','--release','--target','aarch64-linux-android','--manifest-path',path.join(path.resolve(aim),'Cargo.toml'),'--package','composer','--target-dir',target,'-j','2'],
  {cwd:aim,env:{...env,...remaps,...(linker?{CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER:linker}:{}),CARGO_ENCODED_RUSTFLAGS:[remaps.CARGO_ENCODED_RUSTFLAGS,...libraryDirectories.map(directory=>'-Lnative='+path.resolve(directory))].join('\x1f')},stdio:'inherit'});
 return {composerHal:path.join(target,'aarch64-linux-android/release/composer-service')};
}
