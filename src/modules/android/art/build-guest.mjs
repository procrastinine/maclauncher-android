import {prepareWebkitCallbacks} from '../native/webkit-jni-source.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {compileHostObject,compileHostExecutable} from '../native/host-compile.mjs';
import {publisherList} from '../native/binder-rpc/build.mjs';
import {copyChanged} from '../native/upstream/source-files.mjs';
import {prepareArtBinderSource} from './guest-source.mjs';
import {prepareResolverClient} from './resolver-source.mjs';

/** Build the matching original libbinder and the owned JNI/physical boundary.
 * The upstream producer supplies its generated AIDL outputs, compiler flags
 * and exported include directories. Clang/Ninja retain their real header
 * dependencies; this recipe does not evaluate an Android product graph. */
export function buildArtGuest({output,nativeSource,sourceRoot,compiler,linkCompiler=compiler,cCompiler,strip,ninja,python,blueprintParser=process.env.ANDROID_BLUEPRINT_PARSER,binderFlags,cFlags=[],includeDirectories,generatedSources,image,libraryDirectories=[],aidl,targets}){
 output=path.resolve(output);const root=import.meta.dirname,source=path.join(root,'native'),shared=path.resolve(root,'../native');
 const prepared=prepareArtBinderSource({nativeSource,output:path.join(output,'source')});
 const common=[...[prepared.adapter,path.join(prepared.binder,'include'),prepared.binder,source,...includeDirectories].map(file=>'-I'+path.resolve(file)),...binderFlags,'-fPIC'];
 // Imported platform headers keep their original module's warning contract;
 // the owned adapters still compile with the selected compiler's strict flags.
 const adapterFlags=[...common,...includeDirectories.flatMap(file=>['-isystem',path.resolve(file)])];
 const modules=['libbinder_common_defaults','libbinder_android_defaults','libbinder_kernel_defaults','libbinder'];
 const expand=file=>{
  if(file.startsWith(':'))return publisherList(prepared.binder,file.slice(1),'srcs',{parser:blueprintParser}).flatMap(expand);
  if(!file.endsWith('.aidl'))return [path.join(prepared.binder,file)];
  const generated=generatedSources[file];if(!Array.isArray(generated)||!generated.length)throw Error('Missing original Binder generated source provider: '+file);
  return generated.map(value=>path.resolve(value));
 };
 const directory=path.join(output,'objects'),delivery=path.join(output,'guest');fs.mkdirSync(delivery,{recursive:true});
 const object=(file,index,flags=common,driver=compiler)=>compileHostObject({name:index+'-'+path.basename(file)+'.o',inputs:[file],flags,directory,cwd:sourceRoot,ninja,python,compiler:driver,pathRoots:[[nativeSource,"/src/android-native"],[image,"/src/android-image"],...includeDirectories.map((directory,index)=>[directory,"/src/android-headers/"+index])]}).output;
 const libraries=[delivery,path.join(image,'system/lib64'),...libraryDirectories].map(file=>'-L'+path.resolve(file));
 const link=(name,objects,dependencies,sharedLibrary=true)=>{
  // The common Clang planner also supports an ELF driver. Publication uses
  // the matching toolchain's strip, never Darwin install-name/codesign tools.
  const product=compileHostExecutable({name,inputs:objects,flags:['-nostdlib++',...(sharedLibrary?['-shared','-Wl,-soname,'+name]:[]),'-Wl,--no-undefined',...libraries],extra:dependencies,directory,cwd:sourceRoot,ninja,python,compiler:linkCompiler,pathRoots:[[nativeSource,"/src/android-native"],[image,"/src/android-image"]]}).output;
  const target=path.join(delivery,name),temporary=target+'.tmp';
  execFileSync(strip,['--strip-debug','-o',temporary,product]);copyChanged(temporary,target);fs.rmSync(temporary);return target;
 };
 const cflags=[...cFlags,'-O2','-fPIC',...[source,shared,path.join(sourceRoot,'bionic/libdl/include_private'),...includeDirectories].map(file=>'-I'+path.resolve(file))];
 // Component selection follows the same producers and link dependencies as a
 // complete build. It never selects individual upstream source declarations.
 const products={
  binderLibrary(){
   const inputs=[...new Set(modules.flatMap(module=>publisherList(prepared.binder,module,'srcs',{parser:blueprintParser})).flatMap(expand)),...prepared.sources];
   const objects=inputs.map((file,index)=>object(file,'binder-'+index,file.endsWith('RpcSessionBridge.cpp')?[...common,'-fexceptions']:common));
   return link('libbinder.so',objects,['-lcutils','-llog','-lutils','-lbase','-lnativehelper','-lapexsupport','-lvndksupport','-l:libc++.so']);
  },
  logLibrary:()=>link('libandroid-log-stderr.so',[object(path.join(source,'log-stderr.c'),'log',cflags,cCompiler)],['-llog']),
  rpcLibrary(){produce('binderLibrary');produce('logLibrary');return link('libart-rpc.so',[object(path.join(source,'rpc-load.cpp'),'rpc')],['-lbinder','-l:libandroid-log-stderr.so','-llog','-l:libc++.so']);},
  authorityLibrary:()=>link('libart-authority.so',[object(path.join(shared,'supervisor-channel.c'),'authority',cflags,cCompiler)],[]),
  guestLibrary(){
   produce('binderLibrary');
   const webkitCallbacks=prepareWebkitCallbacks(path.join(output,'webkit-callbacks'));
   const androidLinkerApi=path.resolve(image,'apex/com.android.runtime/lib64/bionic/libdl_android.so');
   // The platform boundary remains mapped through process teardown while a
   // native transport worker finishes after its Android session has retired.
   return link('libart-platform.so',[object(path.join(shared,'webkit-jni.cpp'),'webkit-jni',[...adapterFlags,'-I'+shared,'-I'+webkitCallbacks,'-std=c++20','-fexceptions']),object(path.join(source,'webkit-client.cpp'),'webkit-client',[...adapterFlags,'-I'+shared,'-std=c++20','-fexceptions']),object(path.join(shared,'web-links.c'),'web-links-jni',cflags,cCompiler),object(path.join(source,'graphics-guest.cpp'),'graphics',adapterFlags),object(path.join(source,'input-guest.cpp'),'input',adapterFlags),...['guest-jni.c','filesystem-jni.c','web-links-guest.c','network-jni.c','media-jni.c'].map(file=>object(path.join(source,file),file,cflags,cCompiler))],['-Wl,-z,nodelete','-lgui','-landroid_runtime','-laudioclient','-linput','-lbinder','-lbinder_ndk','-lnativewindow','-lvulkan','-landroid','-lutils','-lbase',androidLinkerApi,'-l:libc++.so']);
  },
  dnsPolicyLibrary:()=>link('libcom.android.tethering.dns_helper.so',[object(path.join(source,'dns-policy-guest.c'),'dns-policy',[...cflags,'-I'+path.join(sourceRoot,'packages/modules/Connectivity/bpf/dns_helper/include')],cCompiler)],[]),
  compositionRoot(){
   produce('binderLibrary');
   const resolver=prepareResolverClient({output:path.join(output,'resolver-source'),compiler:aidl,sourceRoot});
   const resolverFlags=[...adapterFlags,'-I'+resolver.include,'-I'+resolver.publicInclude];
   const objects=[path.join(source,'resolver-guest.cpp'),...resolver.sources].map((file,index)=>object(file,'resolver-'+index,resolverFlags));
   return link('android-art-composition',[object(path.join(source,'composition-root.cpp'),'composition',adapterFlags),...objects],['-landroid','-lgui','-laudioclient','-laudiomanager','-lbinder','-lbinder_ndk','-lbinder_rpc_unstable','-lui','-lutils','-lbase','-l:libc++.so'],false);
  },
  serviceWaiter(){produce('binderLibrary');return link('android-art-service-wait',[object(path.join(source,'service-wait.cpp'),'service-wait',adapterFlags)],['-lbinder','-lutils','-l:libc++.so'],false);}
 };
 const result={};
 const produce=name=>{
  if(!Object.hasOwn(products,name))throw Error('Unknown Android guest component: '+name);
  return result[name]??=(products[name]());
 };
 for(const name of targets??Object.keys(products))produce(name);
 return result;
}
