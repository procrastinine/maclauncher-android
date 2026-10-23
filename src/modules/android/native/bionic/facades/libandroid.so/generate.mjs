import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';

const root = import.meta.dirname;
const hash = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const carriers = ['libmaclauncher-ndk-host.so', 'libmaclauncher-graphics-ndk.so',
  'libmaclauncher-looper.so', 'libmaclauncher-choreographer.so'];

function symbols(readelf, file) {
  const output = execFileSync(readelf, ['--dyn-syms', '--wide', file], {encoding:'utf8'});
  return output.split('\n').flatMap(line => {
    const match = line.match(/^\s*\d+:\s+([0-9a-f]+)\s+(\d+)\s+(\w+)\s+(\w+)\s+(\w+)\s+(\w+)\s+(\S+)/);
    if (!match || match[6] === 'UND' || !['GLOBAL','WEAK'].includes(match[4])) return [];
    const [, address, size, type, binding, visibility, section, decorated] = match;
    const [name, version] = decorated.split(/@@?/);
    return [{name, version:version || null, defaultVersion:!version || decorated.includes('@@'),
      type, binding, visibility, address, size:Number(size), section}];
  });
}

/** Refresh only when the pinned original Android library changes. */
export function inventoryLibandroid({elf, readelf, output=path.join(root,'exports.json')}) {
  const functions = symbols(readelf, elf);
  if (functions.length !== 431 || functions.some(row => row.type !== 'FUNC' ||
      row.binding !== 'GLOBAL' || row.visibility !== 'DEFAULT' || !row.defaultVersion ||
      !['LIBANDROID','LIBANDROID_PLATFORM'].includes(row.version)))
    throw Error('The original Android library export contract changed');
  const inventory = {schemaVersion:1, original:{name:'libandroid.so', sha256:hash(elf)},
    versions:['LIBANDROID','LIBANDROID_PLATFORM'],
    functions:functions.sort((a,b)=>a.name<b.name?-1:a.name>b.name?1:0).map(({name,version,binding})=>({name,version,binding}))};
  fs.writeFileSync(output, JSON.stringify(inventory,null,2)+'\n');
  return inventory;
}

/** Match the complete original surface to the actual built typed carriers. */
export function generateLibandroidFacade({guest, readelf, output}) {
  if (!guest || !readelf || !output) throw Error('Expected guest libraries, readelf and output');
  const inventory = JSON.parse(fs.readFileSync(path.join(root,'exports.json')));
  if (inventory.schemaVersion !== 1 || inventory.functions.length !== 431)
    throw Error('The original Android export inventory is invalid');
  const originalNames = new Set(inventory.functions.map(row=>row.name));
  const ownership = new Map(), suppliers = [];
  for (let index=0; index<carriers.length; index++) {
    const library = carriers[index], exported = symbols(readelf,path.join(guest,library));
    const names = new Set(exported.filter(row=>row.type==='FUNC').map(row=>row.name));
    if (!names.size) throw Error('Android typed library has no exported functions: '+library);
    for (const name of names) {
      if (!originalNames.has(name)) continue;
      if (ownership.has(name)) throw Error('Android typed libraries overlap at '+name);
      ownership.set(name,index+1);
    }
    suppliers.push({library,exportedFunctions:names.size});
  }
  const functions = inventory.functions.map(row=>({...row,owner:ownership.get(row.name)||0}));
  for (const row of functions) if (!/^[A-Za-z_]\w*$/.test(row.name))
    throw Error('Unsupported original symbol spelling: '+row.name);
  const literal = value => JSON.stringify(value);
  fs.mkdirSync(output,{recursive:true});
  let code = '#define _GNU_SOURCE 1\n#include <dlfcn.h>\n#include <stddef.h>\n#include <stdio.h>\n#include <stdlib.h>\n';
  code += `__attribute__((visibility("hidden"))) void *klj_public_android_targets[${functions.length}];\n`;
  code += 'static void *owners[5];\nstatic const char *const libraries[]={"/system/lib64/libandroid-platform.so",'+carriers.map(literal).join(',')+'};\n';
  code += 'static const struct {const char *name,*version;unsigned owner;} functions[]={\n';
  code += functions.map(row=>`{${literal(row.name)},${literal(row.version)},${row.owner}},`).join('\n')+'\n};\n';
  code += `static _Noreturn void unavailable(const char *name){fprintf(stderr,"Android native library could not resolve %s\\n",name);abort();}
__attribute__((constructor)) static void prepare(void){
 Dl_info facade;
 if(!dladdr((void*)prepare,&facade)||!facade.dli_fbase)unavailable("libandroid.so");
 for(size_t i=0;i<sizeof(owners)/sizeof(*owners);i++){
  owners[i]=dlopen(libraries[i],RTLD_NOW|RTLD_LOCAL|(i?RTLD_NOLOAD:0));
  if(!owners[i])unavailable(libraries[i]);
 }
 for(size_t i=0;i<sizeof(functions)/sizeof(*functions);i++){
  const unsigned owner=functions[i].owner;
  void *target=owner?dlsym(owners[owner],functions[i].name):dlvsym(owners[0],functions[i].name,functions[i].version);
  if(!target)unavailable(functions[i].name);
  Dl_info resolved;
  if(!dladdr(target,&resolved)||resolved.dli_fbase==facade.dli_fbase)unavailable(functions[i].name);
  klj_public_android_targets[i]=target;
 }
}
__attribute__((destructor)) static void release(void){
 for(size_t i=sizeof(owners)/sizeof(*owners);i>0;i--)if(owners[i-1]){dlclose(owners[i-1]);owners[i-1]=NULL;}
}
`;
  fs.writeFileSync(path.join(output,'facade.c'),code);
  const assembly = ['.text'];
  for (const [index,row] of functions.entries()) assembly.push(`.p2align 2\n.global ${row.name}\n.type ${row.name},%function\n${row.name}:\n bti c\n adrp x16, klj_public_android_targets+${index*8}\n ldr x16, [x16, :lo12:klj_public_android_targets+${index*8}]\n br x16\n.size ${row.name}, .-${row.name}`);
  assembly.push('.section .note.GNU-stack,"",%progbits');
  fs.writeFileSync(path.join(output,'facade.S'),assembly.join('\n')+'\n');
  const versions = inventory.versions.map((version,index)=>`${version} {\n global:\n${functions.filter(row=>row.version===version).map(row=>'  '+row.name+';').join('\n')}\n${index===inventory.versions.length-1?' local: *;\n':''}};`).join('\n');
  fs.writeFileSync(path.join(output,'exports.map'),versions+'\n');
  const routing = {schemaVersion:1,original:inventory.original,versions:inventory.versions,
    suppliers:suppliers.map((row,index)=>({...row,routedFunctions:functions.filter(f=>f.owner===index+1).length})),
    functions:functions.map(({owner,...row})=>({...row,library:owner?carriers[owner-1]:'libandroid-platform.so'}))};
  fs.writeFileSync(path.join(output,'routing.json'),JSON.stringify(routing,null,2)+'\n');
  return {functions:functions.length,typed:functions.filter(row=>row.owner).length,
    original:functions.filter(row=>!row.owner).length,output};
}

if (process.argv[1] && path.resolve(process.argv[1])===fileURLToPath(import.meta.url)) {
  const options={};for(let i=2;i<process.argv.length;i+=2){const key=process.argv[i].replace(/^--/,'');if(!['elf','readelf','guest','output'].includes(key)||!process.argv[i+1])throw Error('Expected --elf, --readelf, --guest or --output');options[key]=process.argv[i+1];}
  const result=options.elf?inventoryLibandroid(options):generateLibandroidFacade(options);
  console.log(JSON.stringify(options.elf?{functions:result.functions.length,versions:result.versions}:result));
}
