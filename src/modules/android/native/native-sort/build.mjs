import {copyOptional,writeChanged} from '../upstream/source-files.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {exportSources} from '../binder-rpc/build.mjs';
import {compileHostObject} from '../host-compile.mjs';

const pins=JSON.parse(fs.readFileSync(new URL('../source-pins.json',import.meta.url)));

/** Original Android sorting keeps its comparator/context ABI on the shared
 * native owner. Darwin's qsort_r places both context arguments differently. */
export function prepareNativeSort({bionicSource,build,ninja,python}){
  const output=path.join(build,'native-sort'),source=path.join(output,'source');
  const directory='libc/upstream-freebsd/lib/libc/stdlib',units=['qsort.c','qsort_r.c'];
  exportSources(bionicSource,pins.aospBionic,[...units.map(name=>directory+'/'+name),'libc/NOTICE'],source);
  const namespace=path.join(output,'boundary.h');
  // Include physical C declarations before namespacing the original function
  // definitions. No Darwin qsort_r declaration enters the Android symbol ABI.
  writeChanged(namespace,'#include <sys/cdefs.h>\n#include <stdlib.h>\n#ifndef __FBSDID\n#define __FBSDID(x)\n#endif\n#ifndef __predict_false\n#define __predict_false(value) __builtin_expect(!!(value),0)\n#endif\n#define qsort klj_sort_qsort\n#define qsort_r klj_sort_qsort_r\n');
  // This original unit uses no declarations from FreeBSD's private header.
  // Its algorithms use the physical standard-C primitives included above.
  writeChanged(path.join(output,'libc_private.h'),'#pragma once\n');
  const flags=['-arch','arm64','-mmacosx-version-min=13.0','-O2','-fPIC','-fno-builtin','-Wall','-Wextra','-Werror','-I'+output,'-include',namespace];
  const compile=(name,input)=>compileHostObject({name,inputs:[input],flags,directory:path.join(build,'host-objects'),cwd:source,ninja,python}).output;
  const objects=units.map(name=>compile('bionic-sort-'+name+'.o',path.join(source,directory,name)));
  const names=[...new Set(objects.flatMap(object=>execFileSync('nm',['-gU',object],{encoding:'utf8'}).split('\n').flatMap(line=>{const match=/\b_klj_sort_(\w+)$/.exec(line);return match?[match[1]]:[]})))].sort();
  const lookup=path.join(output,'lookup.c');
  writeChanged(lookup,'#include <string.h>\n'+names.map(name=>'extern void klj_sort_'+name+'(void);').join('\n')+'\nvoid *klj_sort_lookup(const char *name){\n'+names.map(name=>'if(!strcmp(name,"'+name+'"))return (void *)&klj_sort_'+name+';').join('\n')+'\nreturn 0;\n}\n');
  objects.push(compile('bionic-sort-lookup.o',lookup));
  const notices=path.join(output,'notices');
  copyOptional(path.join(source,'libc/NOTICE'),path.join(notices,'NOTICE'));
  return {output,objects,names,notices};
}
