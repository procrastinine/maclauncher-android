import {prepareNdkSources} from './ndk-sources.mjs';
import fs from'node:fs';import path from'node:path';import{execFileSync}from'node:child_process';
import{publisherList}from'./build.mjs';
import{parseBlueprint}from'../upstream/blueprint.mjs';
export function extendBinder({binder,output,sources,flags,aidl,framework,keymap}){
 const ndk=path.join(output,'ndk');fs.mkdirSync(ndk,{recursive:true});
 prepareNdkSources({work:ndk,binder});
 fs.copyFileSync(ndk+'/include/vm_sockets.h',binder+'/vm_sockets.h');
 const generated=ndk+'/generated';fs.mkdirSync(generated,{recursive:true});
 const inputs=['libbinder_aidl','libbinder_accessor_aidl'].flatMap(name=>publisherList(binder,name,'srcs').map(p=>binder+'/'+p));
 execFileSync(aidl,['--lang=cpp','-I'+binder+'/aidl','-I'+path.join(framework,'core/java'),'--out='+generated,'--header_out='+generated,...inputs],{stdio:'inherit'});
 sources[sources.indexOf(binder+'/Binder.cpp')]=ndk+'/Binder-darwin.cpp';
 const mods=parseBlueprint(fs.readFileSync(binder+'/Android.bp','utf8')).modules;
 const kernel=mods.find(m=>m.properties.name==='libbinder_kernel_defaults').properties;
 const android=mods.find(m=>m.properties.name==='libbinder_android_defaults').properties;
 const all=[...android.target.host.srcs,...kernel.srcs,...kernel.target.host.srcs,...publisherList(binder,'libbinder','srcs')].flatMap(name=>name===':libbinder_device_interface_sources'?publisherList(binder,name.slice(1),'srcs'):name.startsWith(':')?[]:[name]);
 fs.writeFileSync(ndk+'/UtilsHost-darwin.cpp','#include <signal.h>\n'+fs.readFileSync(binder+'/UtilsHost.cpp','utf8'));
 const present=new Set(sources.map(p=>path.basename(p).replace('-darwin','')));
 for(const name of all)if(!present.has(name)){sources.push(name==='UtilsHost.cpp'?ndk+'/UtilsHost-darwin.cpp':name==='BackendUnifiedServiceManager.cpp'?ndk+'/BackendUnifiedServiceManager-darwin.cpp':binder+'/'+name);present.add(name);}
 const ndkMods=parseBlueprint(fs.readFileSync(binder+'/ndk/Android.bp','utf8')).modules;
 const lib=ndkMods.find(m=>m.type==='cc_library'&&m.properties.name==='libbinder_ndk').properties;
 for(const name of [...publisherList(binder+'/ndk','libbinder_ndk_common_defaults','srcs'),...lib.srcs])sources.push(name==='binder_rpc.cpp'?ndk+'/binder_rpc-darwin.cpp':binder+'/ndk/'+name);
 for(const name of fs.readdirSync(generated+'/android/os'))if(name.endsWith('.cpp'))sources.push(generated+'/android/os/'+name);
 flags.unshift('-I'+ndk+'/include', '-I'+keymap);flags.push(...[generated,binder+'/ndk',binder+'/ndk/include_cpp',binder+'/ndk/include_ndk',binder+'/ndk/include_platform'].map(p=>'-I'+p));
 fs.writeFileSync(output+'/extension.json',JSON.stringify({sources,flags},null,2));
}
