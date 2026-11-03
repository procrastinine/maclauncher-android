import {copyOptional,copyChanged} from '../upstream/source-files.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {prepareUnicorn} from './unicorn-source.mjs';
import {nativePathFlags,nativeFlagsString,publishNativeBinary} from '../upstream/native-paths.mjs';

export function buildUnicorn({source,output,work=path.join(output,'work'),cmake='cmake',jobs=4}){
 source=path.resolve(source);output=path.resolve(output);work=path.resolve(work);
 const projection=path.join(work,'source'),build=path.join(work,'build');
 const prepared=prepareUnicorn({source,output:projection});
 const flags=nativeFlagsString(nativePathFlags({source:projection,build,roots:[[source,'/src/unicorn']]}));
 const environment={...process.env,ARCHFLAGS:'-arch arm64'};
 const run=(program,args)=>execFileSync(program,args,{stdio:'inherit',env:environment});
 run(cmake,['-S',projection,'-B',build,'-DCMAKE_BUILD_TYPE=Release','-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0','-DUNICORN_ARCH=aarch64','-DUNICORN_BUILD_TESTS=OFF','-DUNICORN_INSTALL=OFF','-DBUILD_SHARED_LIBS=ON','-DUNICORN_LEGACY_STATIC_ARCHIVE=ON',...['C','ASM'].map(language=>'-DCMAKE_'+language+'_FLAGS='+flags)]);
 run(cmake,['--build',build,'--target','unicorn','--parallel',String(jobs)]);
 const library=path.join(output,'libunicorn.2.dylib');
 publishNativeBinary(path.join(build,'libunicorn.2.dylib'),library,{rpaths:[],installName:'@rpath/libunicorn.2.dylib'});
 const notices=path.join(output,'notices');fs.mkdirSync(notices,{recursive:true});
 for(const file of ['COPYING','COPYING.LGPL2','COPYING_GLIB','qemu/COPYING','qemu/COPYING.LIB','qemu/LICENSE'])copyOptional(path.join(source,file),path.join(notices,'unicorn-'+file.replaceAll('/','-')));
 const identity={schemaVersion:1,...JSON.parse(fs.readFileSync(new URL('./unicorn-source.json',import.meta.url),'utf8')),parallelAtomics:true};
 fs.writeFileSync(path.join(output,'unicorn-build.json'),JSON.stringify(identity,null,2)+'\n');
 return {library,notices,identity,source:projection,build};
}
if(process.argv[1]&&path.resolve(process.argv[1])===fileURLToPath(import.meta.url)){
 const options={};for(let i=2;i<process.argv.length;i+=2){const key=process.argv[i].replace(/^--/,'');if(!['source','output','work'].includes(key)||!process.argv[i+1])throw Error('Expected --source, --output and optional --work paths');options[key]=process.argv[i+1];}
 if(!options.source||!options.output)throw Error('Expected --source and --output paths');buildUnicorn(options);
}
