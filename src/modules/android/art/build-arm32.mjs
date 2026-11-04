import {prepareSourceTree,copyChanged} from '../native/upstream/source-files.mjs';
import path from 'node:path';
import fs from 'node:fs';
import {execFileSync} from 'node:child_process';
import {compileHostObject,compileHostExecutable} from '../native/host-compile.mjs';
import {cachedBuildStep} from '../native/upstream/build-step.mjs';
import {generateLoaderAbi,refreshDescriptors} from '../native/arm32/imports-generate.mjs';
import {generateSignalAbi} from '../native/arm32/signal-abi.mjs';
import {prepareFutexConstants} from '../native/arm32/syscall-sources.mjs';
import {generateJni} from '../native/arm32/jni-generate.mjs';
import {prepareLibraryNamespaces} from '../native/library-namespace-source.mjs';
import {components,prepareArm32Components} from '../native/arm32/components.mjs';

/** Compile the same JNI representation adapter against original Android
 * headers. The CPU archive remains Mach-O; JNI references and libffi closures
 * remain in the executing ART/Bionic context. This is a native link input,
 * not a second library loader or an APK execution capability declaration. */
export async function buildArtArm32Jni({output,ndk,ffi,imports,ninja,python}) {
 const source=import.meta.dirname,shared=path.resolve(source,'../native/arm32');
 output=path.resolve(output);ndk=path.resolve(ndk);
 const toolchain=path.join(ndk,'toolchains/llvm/prebuilt/darwin-x86_64');
 const compiler=path.join(toolchain,'bin/aarch64-linux-android35-clang++');
 const archiver=path.join(toolchain,'bin/llvm-ar');
 const generated=path.join(output,'jni'),objects=path.join(output,'objects');
 prepareSourceTree(generated,temporary=>generateJni({header:path.join(toolchain,'sysroot/usr/include/jni.h'),output:temporary}));
 const flags=['-std=c++20','-O2','-fPIC','-fexceptions','-I'+generated,'-I'+shared,'-I'+path.resolve(ffi.include)];
 const units=[path.join(shared,'jni-bridge.cpp'),path.join(source,'native/arm32-guest.cpp'),path.join(shared,'../proc-maps.c')];
 if(imports){
  refreshDescriptors({output:imports.directory,python,graphicsRegistry:imports.graphicsRegistry});
  generateLoaderAbi({include:path.join(toolchain,'sysroot/usr/include'),output:generated,clang:path.join(toolchain,'bin/clang')});
  generateSignalAbi({include:path.join(toolchain,'sysroot/usr/include'),output:generated,clang:path.join(toolchain,'bin/clang')});
  prepareFutexConstants({include:path.join(toolchain,'sysroot/usr/include'),output:generated,clang:path.join(toolchain,'bin/clang')});
  // Both VMs consume the same complete NDK declaration and syscall outputs.
  flags.push(...[imports.directory,imports.syscalls,imports.format].map(directory=>'-I'+path.resolve(directory)));
  units.push(path.join(shared,'native-imports.cpp'),path.join(shared,'runtime.cpp'),path.join(imports.format,'format-plan.cpp'));
 }
 const pathRoots=[[ndk,'/toolchain/ndk'],[ffi.include,'/src/libffi/include'],...(imports?[['directory','declarations'],['syscalls','syscalls'],['format','format']].map(([name,target])=>[imports[name],'/src/abi/'+target]):[])];
 const inputs=units.map((file,index)=>
  compileHostObject({name:index+'-'+path.basename(file)+'.o',inputs:[file],flags:file.endsWith('.c')?flags.filter(flag=>flag!=='-std=c++20').concat('-std=c11'):flags,directory:objects,cwd:source,ninja,python,compiler:file.endsWith('.c')?path.join(toolchain,'bin/aarch64-linux-android35-clang'):compiler,pathRoots}).output);
 const archive=path.join(output,'libart-arm32-jni.a');
 await cachedBuildStep({cache:path.join(output,'archive-reuse.json'),inputs:[import.meta.filename,archiver,...inputs],files:[archive],key:{operation:'rcs',inputs},
  build:()=>execFileSync(archiver,['rcs',archive,...inputs],{stdio:'inherit'})});
 return {archive,dependencies:[path.resolve(ffi.archive)],include:shared,
  // The NDK adapter uses its original C++ runtime without replacing the
  // matching system image's platform libc++ or any ART framework supplier.
  linkFlags:['-static-libstdc++']};
}

/** Original ART NativeBridge entry around the shared native image owner.
 * This optional supplier is built separately from the default ARM64 payload. */
export async function buildArtNativeBridge(options){
 const {ndk,ffi,imports,ninja,python,bionicSource,artSource,image,arm32}=options;
 const output=path.resolve(options.output),root=import.meta.dirname,shared=path.resolve(root,'../native');
 const adapter=await buildArtArm32Jni({...options,output:path.join(output,'adapter')});
 const original=prepareLibraryNamespaces({bionicSource,artSource,output:path.join(output,'namespace-source')});
 prepareArm32Components(output);
 const bin=path.join(path.resolve(ndk),'toolchains/llvm/prebuilt/darwin-x86_64/bin');
 const directory=path.join(output,'objects'),compiler=path.join(bin,'aarch64-linux-android35-clang++');
 const roots=[[ndk,'/toolchain/ndk'],[image,'/src/android-image'],[ffi.include,'/src/libffi/include']];
 const common=['-O2','-fPIC','-I'+output,'-I'+original.include,'-I'+original.nativeLoaderInclude,'-I'+original.nativeBridgeInclude];
 const objects=[path.join(shared,'library-registry.c'),path.join(shared,'library-group.cpp'),path.join(shared,'library-namespace.cpp'),path.join(root,'native/native-bridge.cpp')].map((file,index)=>
  compileHostObject({name:index+'-'+path.basename(file)+'.o',inputs:[file],flags:[...common,file.endsWith('.c')?'-std=c11':'-std=c++20'],directory,cwd:root,ninja,python,compiler:file.endsWith('.c')?path.join(bin,'aarch64-linux-android35-clang'):compiler,pathRoots:roots}).output);
 // ART releases its bridge before all original pthread TLS destructors have
 // retired. The process owns JNI closures/CPU leases, so keep their code
 // resident through those callbacks using the original ELF lifetime flag.
 const library=compileHostExecutable({name:components.bindings.art,inputs:objects,extra:[adapter.archive,...adapter.dependencies],flags:['-shared','-static-libstdc++','-Wl,--no-undefined','-Wl,-z,nodelete','-Wl,-soname,'+components.bindings.art,'-L'+path.resolve(image,'apex/com.android.runtime/lib64/bionic'),'-ldl_android'],directory,cwd:root,ninja,python,compiler,pathRoots:roots}).output;
 const target=path.join(output,components.bindings.art),temporary=target+'.tmp';
 execFileSync(path.join(bin,'llvm-strip'),['--strip-debug','-o',temporary,library]);copyChanged(temporary,target);fs.rmSync(temporary);
 return {
  nativeBridge:target,
  ...arm32.preloads
 };
}
