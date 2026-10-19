import fs from 'node:fs';
import path from 'node:path';
import {exportSources} from '../binder-rpc/build.mjs';
import {compileHostObject} from '../host-compile.mjs';
import {writeChanged} from '../upstream/source-files.mjs';
import {prepareNativeMultibyte} from './multibyte.mjs';

const pins=JSON.parse(fs.readFileSync(new URL('../source-pins.json',import.meta.url)));

/** The original ctype_private.h table family retains its ARM pointer width.
 * Both original defining units are included whole; their relocations publish
 * the byte-classification and lower/upper-case tables without host aliases. */
export function prepareArm32LegacyCharacterData({bionicSource,output}){
 const files=['libc/upstream-openbsd/lib/libc/gen/ctype_.c','libc/bionic/ndk_cruft_data.cpp',
  'libc/upstream-openbsd/lib/libc/include/ctype_private.h','libc/upstream-openbsd/android/include/openbsd-compat.h','libc/NOTICE'];
 const source=path.join(output,'source');exportSources(bionicSource,pins.aospBionic,files,source);
 const annotations=path.join(output,'annotations.h');
 writeChanged(annotations,'#define __LIBC_HIDDEN__ __attribute__((visibility("hidden")))\n#include "openbsd-compat.h"\n');
 return {units:files.slice(0,2).map(file=>path.join(source,file)),include:path.join(source,'libc/upstream-openbsd/lib/libc/include'),
  includes:[path.join(source,'libc/upstream-openbsd/android/include')],annotations,
  files,notices:files.filter(file=>file.endsWith('.c')||file.endsWith('.cpp')||file.endsWith('.h')).map(file=>path.join(source,file))};
}

/** Bionic's complete inline/out-of-line character family has one shared
 * native owner. Character classification is original Android code; only
 * the compilation namespace and platform declaration macros differ. */
export function prepareNativeCtype({bionicSource,build,ninja,python}){
 build=path.resolve(build);const output=path.join(build,'native-ctype'),source=path.join(output,'source');
 exportSources(bionicSource,pins.aospBionic,['libc/bionic/ctype.cpp','libc/include/ctype.h','libc/NOTICE'],source);
 const header=fs.readFileSync(path.join(source,'libc/include/ctype.h'),'utf8');
 const names=[...new Set([...header.matchAll(/__BIONIC_CTYPE_INLINE\s+int\s+(\w+)\s*\(/g)].map(match=>match[1]))];
 const namespace=path.join(output,'namespace.h');
 writeChanged(namespace,'#include <sys/cdefs.h>\n#include <xlocale.h>\n#define __BIONIC_CAST(kind,type,value) ((type)(value))\n'+names.map(name=>`#undef ${name}\n#define ${name} klj_ctype_${name}`).join('\n')+'\n');
 const flags=['-std=c++20','-arch','arm64','-mmacosx-version-min=13.0','-O2','-fPIC','-fno-builtin','-I'+path.join(source,'libc/include'),'-include',namespace];
 const compile=(name,input,compiler)=>compileHostObject({name,inputs:[input],flags,directory:path.join(build,'host-objects'),cwd:source,ninja,python,compiler}).output;
 const objects=[compile('bionic-ctype.o',path.join(source,'libc/bionic/ctype.cpp'),'clang++')];
 const multibyte=prepareNativeMultibyte({bionicSource,revision:pins.aospBionic,build,ninja,python});
 objects.push(...multibyte.objects);
 const bindings=[...names.map(name=>({name,symbol:'klj_ctype_'+name})),...multibyte.names.map(name=>({name,symbol:multibyte.symbols.get(name)}))];
 const lookup=path.join(output,'lookup.cpp');
 writeChanged(lookup,'#include <cstring>\n'+bindings.map(({symbol})=>`extern "C" void ${symbol}(void);`).join('\n')+'\nextern "C" void *klj_ctype_lookup(const char *name){\n'+bindings.map(({name,symbol})=>`if(!std::strcmp(name,"${name}"))return reinterpret_cast<void *>(&${symbol});`).join('\n')+'\nreturn nullptr;\n}\n');
 objects.push(compile('bionic-ctype-lookup.o',lookup,'clang++'));
 const notices=path.join(output,'notices');fs.mkdirSync(notices,{recursive:true});writeChanged(path.join(notices,'NOTICE'),fs.readFileSync(path.join(source,'libc/NOTICE')));
 writeChanged(path.join(notices,'source.json'),JSON.stringify({project:'Bionic character classification and multibyte conversion',revision:pins.aospBionic,units:['libc/bionic/ctype.cpp','libc/include/ctype.h',...multibyte.units.map(file=>'libc/'+file)],adaptation:'Original complete units and Android state layouts, with one owned symbol namespace and physical errno mapping.'},null,2)+'\n');
 return {output,objects,names:bindings.map(binding=>binding.name),notices};
}
