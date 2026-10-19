import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {copyChanged} from './source-files.mjs';

/** Compiler diagnostics retain useful source identities without recording the
 * developer's checkout, temporary directory or build workspace in binaries. */
export function nativePathMappings({source,build,roots=[]}={}){
  const entries=[...(source?[[source,'/src/upstream']]:[]),...(build?[[build,'/build']]:[]),
    [path.resolve(import.meta.dirname,'..'),'/src/maclauncher'],...roots];
  const mappings=new Map();
  for(const [directory,replacement] of entries){
    const absolute=path.resolve(directory);mappings.set(absolute,replacement);
    if(fs.existsSync(absolute))mappings.set(fs.realpathSync(absolute),replacement);
  }
  // Both Clang and Rust use the last matching mapping.
  return [...mappings].sort((a,b)=>a[0].length-b[0].length);
}
export function nativePathFlags(options){
  return [...nativePathMappings(options).flatMap(([from,to])=>[
    '-ffile-prefix-map='+from+'='+to,'-fdebug-prefix-map='+from+'='+to]),'-ffile-compilation-dir=/build'];
}
export function nativeRustPathEnvironment(options,environment=process.env){
  // Cargo's encoded form preserves arguments containing spaces. Its existing
  // encoded flags take precedence over the whitespace-separated RUSTFLAGS.
  const previous=typeof environment.CARGO_ENCODED_RUSTFLAGS==='string'?
    environment.CARGO_ENCODED_RUSTFLAGS.split('\x1f').filter(Boolean):
    (environment.RUSTFLAGS||'').trim().split(/\s+/).filter(Boolean);
  return {CARGO_ENCODED_RUSTFLAGS:[...previous,...nativePathMappings(options)
    .map(([from,to])=>'--remap-path-prefix='+from+'='+to)].join('\x1f')};
}
export const nativeFlagsString=flags=>flags.map(value=>/^[A-Za-z0-9_./:=+,@%\-]+$/.test(value)?value:"'"+value.replaceAll("'","'\\''")+"'").join(' ');

/** Publish only the Mach-O binary, preserving local compiler debug products.
 * Runtime search paths are supplied by the owner of the library closure. */
export function publishNativeBinary(source,destination,{rpaths,installName}={}){
  fs.mkdirSync(path.dirname(destination),{recursive:true});
  const temporary=fs.mkdtempSync(path.join(path.dirname(destination),'.native-publication-'));
  const file=path.join(temporary,path.basename(destination));
  try{
    fs.copyFileSync(source,file,fs.constants.COPYFILE_FICLONE);
    const edits=installName?['-id',installName]:[];
    if(rpaths){
      const commands=execFileSync('otool',['-l',file],{encoding:'utf8'});
      const current=[...commands.matchAll(/cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset \d+\)/g)].map(match=>match[1]);
      edits.push(...current.filter(value=>!rpaths.includes(value)).flatMap(value=>['-delete_rpath',value]),
        ...rpaths.filter(value=>!current.includes(value)).flatMap(value=>['-add_rpath',value]));
    }
    if(edits.length)execFileSync('install_name_tool',[...edits,file],{stdio:'inherit'});
    execFileSync('strip',['-S',file],{stdio:'inherit'});
    execFileSync('codesign',['--force','--sign','-',file],{stdio:['ignore','ignore','pipe']});
    copyChanged(file,destination);
  }finally{fs.rmSync(temporary,{recursive:true,force:true});}
}
