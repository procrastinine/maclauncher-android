import fs from 'node:fs';
import path from 'node:path';
import {writeChanged} from './upstream/source-files.mjs';

// Discover the entire pinned GLES declaration universe. Clang evaluates the
// original function types; no app references or maintained command list enter
// native resolution. Undeclared optional exports retain their provider pointer.
export function prepareGlesGuestAbi(headers,build){
 const files=['GLES/gl.h','GLES/glext.h','GLES3/gl32.h','GLES2/gl2ext.h'];
 const names=[...new Set(files.flatMap(file=>[...fs.readFileSync(path.join(headers,'include',file),'utf8').matchAll(/^GL_API(?:CALL)?\s+[^\n;]+?\bGL_APIENTRY\s+(gl\w+)\s*\(/gm)].map(match=>match[1])))].sort();
 if(!names.length)throw Error('The original GLES declarations are missing');
 const output=path.join(build,'gles-guest-abi.cpp');
 writeChanged(output,'#define GL_GLEXT_PROTOTYPES 1\n#define GL_GLES_PROTOTYPES 1\n'+files.map(file=>'#include <'+file+'>').join('\n')+'\n#include '+JSON.stringify(path.join(import.meta.dirname,'gles-guest-abi.hpp'))+'\n#include <cstring>\n'+
  names.map(name=>`static constexpr char name_${name}[]="${name}";`).join('\n')+
  '\nextern "C" void *klj_gles_guest_abi(const char *name,void *function){\nif(!function)return nullptr;\n'+
  names.map(name=>`if constexpr(klj_gles_abi::Entry<decltype(&::${name}),name_${name}>::needs)if(!std::strcmp(name,name_${name}))return klj_gles_abi::Entry<decltype(&::${name}),name_${name}>::bind(function);`).join('\n')+
  '\nreturn function;\n}\n');
 return output;
}
