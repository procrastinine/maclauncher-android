import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {buildUnicorn} from './unicorn-build.mjs';
import {nativePathFlags} from '../upstream/native-paths.mjs';
import {copyChanged} from '../upstream/source-files.mjs';

/** Static component; consumers do not need a separate CPU library at launch. */
export function buildPrecise({source,output,work=path.join(output,'work'),unicornWork=path.join(work,'unicorn'),jobs=4}){
 const dependency=buildUnicorn({source,output:path.join(output,'unicorn'),work:unicornWork,jobs});
 execFileSync('cmake',['--build',dependency.build,'--target','unicorn_archive','--parallel',String(jobs)],{stdio:'inherit'});
 const here=path.dirname(fileURLToPath(import.meta.url)),object=path.join(work,'precise.o'),archive=path.join(output,'libkl_precise.a');
 fs.mkdirSync(work,{recursive:true});fs.mkdirSync(output,{recursive:true});
 execFileSync('clang++',['-std=c++20','-O2','-g0','-fPIC','-arch','arm64','-mmacosx-version-min=13.0',...nativePathFlags({source:here,build:work,roots:[[source,'/src/unicorn']]}),'-I'+path.join(source,'include'),'-c',path.join(here,'precise.cpp'),'-o',object],{stdio:'inherit'});
 execFileSync('ar',['crs',archive,object],{stdio:'inherit'});
 const unicorn=path.join(output,'libunicorn.a');copyChanged(path.join(dependency.build,'libunicorn.a'),unicorn);
 const result={schemaVersion:1,archives:[archive,unicorn],notices:dependency.notices,upstream:dependency.identity};
 fs.writeFileSync(path.join(output,'precise-build.json'),JSON.stringify(result,null,2)+'\n');return result;
}
if(process.argv[1]&&path.resolve(process.argv[1])===fileURLToPath(import.meta.url)){
 const options={};for(let i=2;i<process.argv.length;i+=2){const key=process.argv[i].replace(/^--/,'');if(!['source','output','work'].includes(key)||!process.argv[i+1])throw Error('Expected --source, --output and optional --work paths');options[key]=process.argv[i+1];}
 if(!options.source||!options.output)throw Error('Expected --source and --output paths');buildPrecise(options);
}
