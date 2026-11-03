import {copyOptional,prepareSourceTree,copyChanged,writeChanged} from '../upstream/source-files.mjs';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import os from 'node:os';
import {execFileSync} from 'node:child_process';
import {prepareAimSource} from '../../art/source.mjs';
import {prepareBionicThreads} from './thread-source.mjs';
import {prepareBionicProcess} from './process-source.mjs';
import {generate} from './jni-generate.mjs';
import {nativePathFlags,nativeRustPathEnvironment,publishNativeBinary} from '../upstream/native-paths.mjs';
import {compileHostLibrary} from '../host-compile.mjs';
import {generateLibandroidFacade} from './facades/libandroid.so/generate.mjs';

const root=import.meta.dirname,sha=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const families=[['ndk','libmaclauncher-ndk-host.so'],['graphics','libmaclauncher-graphics-ndk.so'],['looper','libmaclauncher-looper.so'],['egl','libmaclauncher-egl.so'],['aaudio','libmaclauncher-aaudio.so'],['choreographer','libmaclauncher-choreographer.so']];
const supplierPaths={linker:'guest/linker64',inodeBootstrap:'android-inode-bootstrap',linkerConfigRunner:'linkerconfig-runner',imageExtractor:'android-image-extract'};
const run=(command,args,options={})=>execFileSync(command,args,{stdio:'inherit',...options});
function relative(value){
 if(typeof value!=='string'||!value||path.isAbsolute(value)||value.split(/[\\/]/).some(part=>!part||part==='.'||part==='..'))throw Error('Invalid Bionic supplier member');
 return value;
}

/** Supplied native components are verified inputs, like ANGLE and codecs.
 * Their source identities and notices travel with the runtime; source builders
 * can provide the same files without changing this runtime or loading policy. */
export function readBionicInputs(file){
 const directory=path.dirname(path.resolve(file)),value=JSON.parse(fs.readFileSync(file,'utf8'));
 if(value.schemaVersion!==1||!value.systemImage?.acquisition?.sha256||!value.systemImage?.content?.sha256)throw Error('Invalid Bionic supplier input manifest');
 const artifacts={};
 for(const [name,destination]of Object.entries(supplierPaths)){
  const entry=value.artifacts?.[name];
  if(!entry||!/^([a-f0-9]{64})$/.test(entry.sha256)||!entry.source||typeof entry.file!=='string')throw Error('Missing Bionic '+name+' supplier identity');
  const source=path.resolve(directory,entry.file),stat=fs.lstatSync(source);
  if(!stat.isFile()||sha(source)!==entry.sha256)throw Error('Bionic '+name+' supplier differs from its declared bytes');
  artifacts[name]={...entry,file:source,destination};
 }
 const notices=[].concat(value.notices||[]).map(entry=>({...entry,file:typeof entry?.file==='string'?path.resolve(directory,entry.file):undefined}));
 return {...value,artifacts,notices};
}

export function bionicBindingInputs({build,familiesOutput=root}){
 const generated=path.join(build,'bionic/jni');
 return {
  sources:[path.resolve(root,'../image-loader.c'),...['runtime-host.c','jni-provider.c','jni-carrier.c','call-frame.c','call-frame.S','host-bootstrap.c'].map(name=>path.join(root,name)),path.join(generated,'bionic-jni-host.c'),
   path.join(root,'ndk/ndk-host.c'),...families.map(([name])=>path.join(familiesOutput,name,name+'-host-generated.c'))],
  flags:['-I'+root,'-I'+generated,...families.map(([name])=>'-I'+path.join(familiesOutput,name)),...families.map(([name])=>'-I'+path.join(root,name)),
   '-I'+path.join(root,'ndk/include'),'-I'+path.join(root,'graphics/include'),'-I'+path.join(root,'graphics/host-include'),'-I'+path.join(root,'egl/include'),'-I'+path.join(root,'aaudio/include'),'-I'+path.join(root,'choreographer/include'),'-DEGL_NO_PLATFORM_SPECIFIC_TYPES']
 };
}

export function generateBionicFamilies({ast,output}){
 for(const [name]of families)run(process.execPath,[path.join(root,name,'generate.mjs'),ast,path.join(output,name)]);
 return output;
}

/** A separate projection retains the original Cargo graph and original guest
 * execution policy. It never mutates an ART build or its supplied checkout. */
export function prepareBionicAim({source,output,klepton,glesHeaders}){
 return prepareSourceTree(output,temporary=>{
  prepareAimSource({source,output:temporary,klepton,glesHeaders,mutableCode:false});
  prepareBionicThreads(temporary);prepareBionicProcess(temporary);
 }),output;
}

export function buildBionicAbi({source,build,output,rustToolchain,target,env=process.env}){
 target=path.resolve(target||path.join(build,'cargo'));
 const cargo=path.join(rustToolchain,'bin/cargo'),cargoHome=env.CARGO_HOME||path.join(os.homedir(),'.cargo');
 const environment={...env,PATH:path.join(rustToolchain,'bin')+path.delimiter+env.PATH,
  RUSTC:path.join(rustToolchain,'bin/rustc'),CARGO_NET_OFFLINE:'true',MACOSX_DEPLOYMENT_TARGET:'13.0'};
 // The ordinary ABI and this embedded ABI use their original native execution
 // model. Experimental CPU archives are not an input to the embedded target.
 delete environment.MACLAUNCHER_PRECISE_ARCHIVES;
 Object.assign(environment,nativeRustPathEnvironment({source,build:target,roots:[[root,'/src/bionic'],[cargoHome,'/src/cargo'],[rustToolchain,'/toolchain/rust']]},environment));
 run(cargo,['build','--offline','--locked','--release','-j','2','--manifest-path',path.join(source,'Cargo.toml'),'--target-dir',target,'-p','aim-linux-abi','--lib'],{cwd:source,env:environment});
 const library=path.join(output,'libaim_linux_abi.dylib');
 publishNativeBinary(path.join(target,'release/libaim_linux_abi.dylib'),library,{installName:'@rpath/libaim_linux_abi.dylib'});
 return {library,sourceRevision:JSON.parse(fs.readFileSync(path.resolve(root,'../../art/supplier.json'),'utf8')).sources.aim.commit};
}

/** Build every guest carrier against the same original C/JNI headers. Each
 * public GLES library keeps its complete original export set and soname. */
export function buildBionicGuests({ndk,jdk,build,output,familiesOutput=root}){
 const tools=path.join(ndk,'toolchains/llvm/prebuilt/darwin-x86_64/bin'),clang=path.join(tools,'aarch64-linux-android35-clang');
 const generated=path.join(build,'bionic/jni'),guest=path.join(output,'guest');fs.mkdirSync(guest,{recursive:true});
 generate(generated,jdk);
 const include=['-I'+root,'-I'+generated,...families.map(([name])=>'-I'+path.join(familiesOutput,name)),...families.map(([name])=>'-I'+path.join(root,name)),
  '-I'+path.join(root,'ndk/include'),'-I'+path.join(root,'graphics/include'),'-I'+path.join(root,'egl/include'),'-I'+path.join(root,'aaudio/include'),'-I'+path.join(root,'choreographer/include'),
  '-I'+path.join(jdk,'include'),'-I'+path.join(jdk,'include/darwin'),'-DEGL_NO_PLATFORM_SPECIFIC_TYPES'];
 const flags=['-fPIC','-O2','-Wall','-Wextra','-Werror','-Wno-nullability-completeness',...nativePathFlags({source:root,build,roots:[[ndk,'/toolchain/ndk'],[jdk,'/toolchain/jdk']]}),...include];
 const products=[];
 for(const [family,name]of families){
  const file=path.join(guest,name);
  run(clang,['-shared',...flags,path.join(familiesOutput,family,family+'-guest-generated.c'),'-Wl,-z,global','-Wl,-soname,'+(family==='aaudio'?'libaaudio.so':name),'-o',file]);products.push(file);
 }
 const bootstrap=path.join(guest,'bootstrap.so');
 run(clang,[...flags,path.join(root,'runtime-guest.c'),path.join(root,'call-packet.c'),path.join(root,'call-packet.S'),path.join(generated,'bionic-jni-guest.c'),path.join(generated,'bionic-jni-calls.S'),
  '-L'+guest,'-Wl,--no-as-needed',...families.map(([,name])=>'-l:'+name),'-Wl,-rpath,/data/local/tmp/maclauncher-bionic','-ldl','-Wl,--export-dynamic','-o',bootstrap]);products.push(bootstrap);
 const androidFacade=path.join(build,'bionic/facades/libandroid.so');
 generateLibandroidFacade({guest,readelf:path.join(tools,'llvm-readelf'),output:androidFacade});
 for(const name of ['libGLESv2.so','libGLESv3.so','libandroid.so']){
  const directory=name==='libandroid.so'?androidFacade:path.join(root,'facades',name),file=path.join(guest,name);
  run(clang,['-shared',...flags,path.join(directory,'facade.c'),path.join(directory,'facade.S'),'-L'+guest,...(name==='libandroid.so'?[]:['-Wl,--no-as-needed','-lmaclauncher-egl']),'-ldl','-Wl,--no-undefined','-Wl,-soname,'+name,'-Wl,--version-script='+path.join(directory,'exports.map'),'-Wl,-z,relro','-Wl,-z,now','-o',file]);products.push(file);
 }
 return {products,generated};
}

export function stageBionicSuppliers({suppliers,output,notices}){
 fs.mkdirSync(output,{recursive:true});fs.mkdirSync(notices,{recursive:true});
 for(const entry of Object.values(suppliers.artifacts))copyChanged(entry.file,path.join(output,entry.destination));
 for(const entry of suppliers.notices)copyOptional(entry.file,path.join(notices,entry.path));
 const identity={schemaVersion:1,systemImage:suppliers.systemImage,artifacts:Object.fromEntries(Object.entries(suppliers.artifacts).map(([key,{sha256,source}])=>[key,{sha256,source}]))};
 const profile={schemaVersion:1,nativeAbi:'arm64-v8a',aaudioVersion:1,choreographerVersion:1,libandroidFacadeVersion:1,inodeBootstrap:'native/bionic/android-inode-bootstrap',inodeBootstrapSha256:suppliers.artifacts.inodeBootstrap.sha256,
  systemImage:{...suppliers.systemImage,extractor:'native/bionic/android-image-extract'},linkerConfigRunner:'native/bionic/linkerconfig-runner',linkerConfigRunnerSha256:suppliers.artifacts.linkerConfigRunner.sha256};
 writeChanged(path.join(output,'profile.json'),JSON.stringify(profile,null,2)+'\n');
 return identity;
}

/** Optional at producer configuration time; legacy and ARM32 calls keep their
 * existing native image provider. The profile selects only genuine ARM64. */
export function buildBionic({options,root:nativeRoot,build,output,flags,pathRoots=[]}){
 if(!options['bionic-inputs'])return null;
 if(!options.ndk)throw Error('The Bionic component requires --ndk');
 const suppliers=readBionicInputs(options['bionic-inputs']),directory=path.join(output,'native/bionic');
 fs.mkdirSync(directory,{recursive:true});
 const aim=prepareBionicAim({source:options['aim-source'],output:path.join(build,'bionic/aim'),klepton:options.klepton,glesHeaders:options['gles-headers']});
 const abi=buildBionicAbi({source:aim,build:path.join(build,'bionic'),output:directory,rustToolchain:options['rust-toolchain'],target:options['bionic-target']});
 const familiesOutput=generateBionicFamilies({ast:path.join(build,'arm32/imports/host-ast.json'),output:path.join(build,'bionic/families')});
 const guest=buildBionicGuests({ndk:options.ndk,jdk:options.jdk,build,output:directory,familiesOutput});
 const identity=stageBionicSuppliers({suppliers,output:directory,notices:path.join(output,'notices/bionic')});
 compileHostLibrary({name:'jsig-bridge.dylib',inputs:[path.join(root,'jsig-bridge.c')],flags,extra:[],directory:path.join(build,'host-objects'),output:path.join(directory,'jsig-bridge.dylib'),cwd:options.klepton,ninja:options.ninja,python:options.python,pathRoots});
 const notice=path.join(output,'notices/bionic/aim');fs.mkdirSync(notice,{recursive:true});
 for(const name of ['LICENSE','NOTICE','LICENSING.md','THIRD_PARTY_NOTICES.md'])if(fs.existsSync(path.join(aim,name)))copyOptional(path.join(aim,name),path.join(notice,name));
 return {binding:bionicBindingInputs({build,familiesOutput}),identity:{...identity,abiSourceRevision:abi.sourceRevision,jni:JSON.parse(fs.readFileSync(path.join(guest.generated,'jni-carrier-manifest.json'))),families:['android-assets-configuration-image-decoder','android-bitmap-hardware-buffer-native-window','android-looper','egl-gles','aaudio','android-choreographer']}};
}
