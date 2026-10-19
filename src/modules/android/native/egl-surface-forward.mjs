import fs from 'node:fs';
import path from 'node:path';
// Preserve every surface/config-taking entry in the pinned Khronos declarations while
// translating the Android window identity to its current ANGLE buffer.
export function generateEglSurfaceForward(headers,output){
 const methods=new Map();
 for(const file of ['egl.h','eglext.h']){
  const text=fs.readFileSync(path.join(headers,'include/EGL',file),'utf8');
  for(const match of text.matchAll(/EGLAPI\s+(.+?)\s+EGLAPIENTRY\s+(egl\w+)\s*\(([^;]+)\);/g)){
   const [,result,name,parameters]=match,args=parameters.split(',').map(value=>value.trim());
   if(!args.some(value=>/^(?:EGLSurface|EGLConfig)\s+\w+$/.test(value)))continue;
   const names=args.map(value=>{const found=value.match(/([A-Za-z_]\w*)\s*$/);if(!found)throw Error('Invalid EGL parameter: '+value);return found[1];});
   const translated=args.map((value,index)=>/^EGLSurface\s+/.test(value)?`surfaceRaw(${names[index]})`:/^EGLConfig\s+\w+$/.test(value)?`configRaw(${names[index]})`:names[index]);
   const signature=`${result}(${args.join(',')})`;
   if(methods.has(name)&&methods.get(name).signature!==signature)throw Error('Conflicting EGL declaration: '+name);
   methods.set(name,{name,signature,result,args,translated});
  }
 }
 if(!methods.has('eglMakeCurrent')||!methods.has('eglSwapBuffersWithDamageKHR'))throw Error('Missing EGL surface declarations');
 const entries=[...methods.values()].sort((a,b)=>a.name.localeCompare(b.name));
 const body=entries.map(({name,result,args,translated})=>`static ${result} EGLAPIENTRY forward_${name}(${args.join(', ')}) {\n using F=${result}(EGLAPIENTRY*)(${args.join(', ')});\n auto call=reinterpret_cast<F>(symbol("${name}"));\n if(!call){error=EGL_BAD_ACCESS;return {}; }\n return call(${translated.join(', ')});\n}`).join('\n');
 fs.writeFileSync(output,'// Generated from the pinned EGL declarations.\n'+body+'\nstatic void *surfaceForward(const char *name){\n'+entries.map(({name})=>` if(!std::strcmp(name,"${name}"))return symbol(name)?reinterpret_cast<void*>(forward_${name}):nullptr;`).join('\n')+'\n return nullptr;\n}\n');
 return entries.map(({name})=>name);
}
