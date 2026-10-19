import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {compileHostLibrary,compileHostObject} from '../native/host-compile.mjs';
import {prepareNativeSurfaceSources} from '../native/native-surface-sources.mjs';
import {nativeRustPathEnvironment,publishNativeBinary} from '../native/upstream/native-paths.mjs';
import {prepareHostSource} from './source.mjs';
import {prepareSourceTree} from '../native/upstream/source-files.mjs';
import {prepareNetworkPolicy} from '../native/network-policy-source.mjs';

/** Build the VM-neutral Mac operations from their original object dependencies.
 * Native component changes reuse the separately supplied Rust desktop tools. */
export function buildArtHostLibrary({output,klepton,sdl,jni,ninja,python,arm32,minimumMacOS='13.0'}){
 const root=import.meta.dirname,native=path.resolve(root,'../native'),source=path.join(root,'native');
 output=path.resolve(output);const objects=path.join(output,'objects'),delivery=path.join(output,'host');fs.mkdirSync(delivery,{recursive:true});
 const projected=path.join(output,'audio-source');
 prepareSourceTree(projected,temporary=>prepareNativeSurfaceSources(klepton,temporary));
 const policy=path.join(output,'network-source');prepareSourceTree(policy,prepareNetworkPolicy);
 const audioFlags=['-DKLEPTON_AUDIO_PAUSE_SOURCE='+JSON.stringify(path.join(projected,'audio-source.c')),'-DKLEPTON_AUDIO_CAPTURE_STATE='+JSON.stringify(path.join(projected,'capture-state.h')),'-DKLEPTON_AUDIO_CAPTURE_SOURCE='+JSON.stringify(path.join(projected,'capture-original.inc'))];
 const deployment='-mmacosx-version-min='+minimumMacOS;
 const common=['-O2','-fPIC',deployment,'-I'+native,'-I'+source,'-I'+path.join(klepton,'runtime'),'-I'+path.join(klepton,'runtime/media'),'-I'+path.join(jni,'include'),'-I'+path.join(jni,'include/darwin'),'-F'+path.dirname(sdl),...audioFlags];
 common.push('-fblocks','-I'+policy);
 if(arm32)common.push('-DKLJ_ART_ARM32=1');
 const units=[...['audio-host.c','web-links-host.c','network-host.c','media-host.c','host-module.c','window-host.mm','webkit-server.mm'].map(file=>path.join(source,file)),
  ...['application-sleep.mm','process-memory.c','process-ids.c','thread-identity.c','audio-device.c','web-links-platform.c','audio-capture.c','media-platform.mm','webkit-platform.mm','webkit-binding.mm','audio-pause-overlay.c','audio-pcm.c','audio-volume.c','network-policy.c','display-snapshot.mm','clock/display-clock.mm','session-core.c'].map(file=>path.join(native,file)),path.join(klepton,'runtime/kl_env.c')];
 if(arm32)units.push(path.join(source,'arm32-host.cpp'));
 units.push(path.join(native,'android-log.c'));
 const inputs=units.map((file,index)=>compileHostObject({name:index+'-'+path.basename(file)+'.o',inputs:[file],flags:[...common,...(file.endsWith('.mm')?['-std=c++20','-fobjc-arc']:file.endsWith('.cpp')?['-std=c++20']:['-std=c11'])],directory:objects,cwd:root,ninja,python,compiler:/\.(cpp|mm)$/.test(file)?'clang++':'clang',pathRoots:[[klepton,'/src/klepton']]}).output);
 if(arm32)inputs.push(arm32.archive,...arm32.dependencies);
 const library=path.join(delivery,'libandroid-art-host.dylib');
 compileHostLibrary({name:path.basename(library),inputs,flags:[deployment,'-F'+path.dirname(sdl)],extra:['-framework','SDL3','-framework','Metal','-framework','QuartzCore','-framework','CoreVideo','-framework','AppKit','-framework','WebKit','-framework','UniformTypeIdentifiers','-framework','Carbon','-framework','Foundation','-framework','CoreAudio','-framework','AudioToolbox','-framework','AVFoundation','-framework','CoreMedia','-framework','Network','-framework','SystemConfiguration','-framework','IOKit','-Wl,-rpath,@loader_path/../../native'],directory:objects,output:library,cwd:root,ninja,python,compiler:'clang++'});
 return {hostLibrary:library};
}

/** Complete native host and original AIM desktop-tool supplier. */
export function buildArtHost(options){
 const {output:requestedOutput,aim,klepton,cargo='cargo',env=process.env}=options;
 const output=path.resolve(requestedOutput),root=import.meta.dirname,delivery=path.join(output,'host');
 const {hostLibrary}=buildArtHostLibrary(options);
 const rust=prepareHostSource({aim,output:path.join(output,'rust-source')}),target=path.join(output,'cargo');
 execFileSync(cargo,['build','--offline','--manifest-path',path.join(rust,'Cargo.toml'),'--target-dir',target,'--release','-j','2'],{cwd:rust,env:{...env,...nativeRustPathEnvironment({source:root,build:output,roots:[[aim,"/src/aim"],[klepton,"/src/klepton"],...(env.CARGO_HOME?[[env.CARGO_HOME,"/src/cargo"]]:[])]},env)},stdio:'inherit'});
 for(const name of ['android-art-properties','android-art-presenter'])publishNativeBinary(path.join(target,'release',name),path.join(delivery,name));
 return {hostLibrary,bootstrap:path.join(delivery,'android-art-properties'),presenter:path.join(delivery,'android-art-presenter')};
}
