import fs from 'node:fs';
import path from 'node:path';
import {generateImports} from '../arm32/imports-generate.mjs';

// The complete published NDK declaration set determines which imports borrow
// a JNI environment. No API names or game libraries are selected manually.
export function emitNdkJniImports(declarations, output) {
  const rows=[];
  for(const method of Object.values(declarations.functions)) {
    const signature=declarations.types[method.type];
    let general=0;
    for(const parameter of signature.parameters) {
      const type=declarations.types[parameter];
      if(type.kind==='Pointer'&&type.element==='JNIEnv') {
        if(signature.variadic||general>=8)throw Error('NDK environment requires a stack ABI adaptation: '+method.symbol);
        rows.push({name:method.symbol,argument:general});
      }
      if(!['Float','Double'].includes(type.kind))general+=Math.ceil(type.size/8);
    }
  }
  rows.sort((a,b)=>a.name.localeCompare(b.name));
  fs.writeFileSync(path.join(output,'ndk-jni-imports.h'),
    '// Generated from the complete original NDK Clang declarations.\n'+
    'static const struct {const char *name;unsigned argument;} ndk_jni_imports[]={\n'+
    rows.map(row=>`{${JSON.stringify(row.name)},${row.argument}},`).join('\n')+'\n};\n');
  return rows;
}

export async function prepareNdkJni({declarationsDirectory,include,python,output}) {
  fs.mkdirSync(output,{recursive:true});
  if(!declarationsDirectory) {
    declarationsDirectory=path.join(output,'native-import-declarations');
    await generateImports({include,python,output:declarationsDirectory});
  }
  const rows=emitNdkJniImports(JSON.parse(fs.readFileSync(path.join(declarationsDirectory,'host-ast.json'))),output);
  return {source:path.join(import.meta.dirname,'adapter.c'),licenses:path.join(declarationsDirectory,'licenses'),declarationsDirectory,rows};
}
