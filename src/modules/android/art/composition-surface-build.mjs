import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {compileHostObject,compileHostExecutable} from '../native/host-compile.mjs';

/** Public NDK surface/buffer boundary; no private libgui build is required. */
export function buildCompositionSurface({ndk,image,output,ninja,python}){
 ndk=path.resolve(ndk);image=path.resolve(image);output=path.resolve(output);
 const compiler=path.join(ndk,'toolchains/llvm/prebuilt/darwin-x86_64/bin/clang');
 const directory=path.join(output,'objects'),source=path.join(import.meta.dirname,'native/composition-surface.c');
 const flags=['--target=aarch64-linux-android36','-O2','-fPIC','-std=c11','-Wall','-Wextra','-Werror'];
 const object=compileHostObject({name:'composition-surface.o',inputs:[source],flags,directory,cwd:import.meta.dirname,ninja,python,compiler}).output;
 const name='libmaclauncher-composition-surface.so',library=path.join(output,name);fs.mkdirSync(output,{recursive:true});
 const product=compileHostExecutable({name,inputs:[object],flags:['--target=aarch64-linux-android36','-shared','-nostdlib','-Wl,--no-undefined','-Wl,-soname,'+name],extra:[path.join(image,'system/lib64/libandroid.so'),path.join(image,'apex/com.android.runtime/lib64/bionic/libc.so')],directory,cwd:import.meta.dirname,ninja,python,compiler}).output;
 const temporary=library+'.tmp';execFileSync(path.join(path.dirname(compiler),'llvm-strip'),['--strip-debug','-o',temporary,product]);fs.renameSync(temporary,library);
 return {library};
}
