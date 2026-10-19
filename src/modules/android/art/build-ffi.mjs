import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {cachedBuildStep} from '../native/upstream/build-step.mjs';
import {nativePathFlags,nativeFlagsString} from '../native/upstream/native-paths.mjs';

/** The original libffi producer supplies executable Android JNI closures.
 * Its configure checks and Make dependency graph own architecture selection. */
export async function buildArtFfi({source,revision,output,ndk,autoreconf='autoreconf',make='make',env=process.env}){
 const pin=JSON.parse(fs.readFileSync(new URL('./supplier.json',import.meta.url))).sources.libffi;
 revision??=pin.commit;
 source=path.resolve(source);output=path.resolve(output);ndk=path.resolve(ndk);
 const run=(program,args,options={})=>execFileSync(program,args,{env,stdio:'inherit',...options});
 const actual=run('git',['-C',source,'rev-parse','HEAD'],{encoding:'utf8',stdio:'pipe'}).trim();
 if(!revision||actual!==revision)throw Error('The libffi source revision differs from the selected supplier');
 const members=run('git',['-C',source,'ls-files','-z'],{encoding:'utf8',stdio:'pipe'}).split('\0').filter(Boolean);
 const tool=name=>fs.realpathSync(path.isAbsolute(name)?name:run('/usr/bin/which',[name],{encoding:'utf8',stdio:'pipe'}).trim());
 const tools={autoreconf:tool(autoreconf),make:tool(make)},bin=path.join(ndk,'toolchains/llvm/prebuilt/darwin-x86_64/bin');
 const cc=path.join(bin,'aarch64-linux-android35-clang'),cxx=path.join(bin,'aarch64-linux-android35-clang++'),ar=path.join(bin,'llvm-ar'),ranlib=path.join(bin,'llvm-ranlib');
 const projected=path.join(output,'source'),build=path.join(output,'build'),install=path.join(output,'install');
 const flags=nativeFlagsString(['-O2','-fPIC',...nativePathFlags({source:projected,build:output,roots:[[source,'/src/libffi']]})]);
 const configure=['--host=aarch64-linux-android','--disable-shared','--enable-static','--disable-docs','--prefix='+install];
 const environment={...env,CC:cc,CXX:cxx,AR:ar,RANLIB:ranlib,CFLAGS:flags,CXXFLAGS:flags,CPPFLAGS:'',LDFLAGS:''};
 await cachedBuildStep({cache:path.join(output,'reuse.json'),inputs:[import.meta.filename,...members.map(name=>path.join(source,name)),...Object.values(tools),cc,cxx,ar,ranlib],
  trees:[install],key:{revision,configure,flags,ndk},build:()=>{
   for(const directory of [projected,build]){fs.rmSync(directory,{recursive:true,force:true});fs.mkdirSync(directory,{recursive:true});}
   for(const name of members){const destination=path.join(projected,name);fs.mkdirSync(path.dirname(destination),{recursive:true});fs.copyFileSync(path.join(source,name),destination);}
   run(tools.autoreconf,['-fi'],{cwd:projected});
   run(path.join(projected,'configure'),configure,{cwd:build,env:environment});
   run(tools.make,['-j2','install'],{cwd:build,env:environment});
  }});
 return {archive:path.join(install,'lib/libffi.a'),include:path.join(install,'include'),license:path.join(source,'LICENSE'),revision};
}
