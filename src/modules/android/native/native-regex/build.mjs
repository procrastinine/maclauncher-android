import {writeChanged} from '../upstream/source-files.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {exportSources} from '../binder-rpc/build.mjs';
import {compileHostObject} from '../host-compile.mjs';

const pins=JSON.parse(fs.readFileSync(new URL('../source-pins.json',import.meta.url)));
const directory='libc/upstream-netbsd/lib/libc/regex/';

/** Original complete libc_netbsd regex target and Android public record.
 * Byte classification and locale remain with the existing native libc owners. */
export function prepareRegexSources({bionicSource,output}){
  const source=path.join(output,'source'),names=['regcomp','regexec','regerror','regfree'];
  const relativeUnits=names.map(name=>directory+name+'.c');
  const headers=['engine.c','utils.h','regex2.h','cname.h'].map(name=>directory+name);
  const support='libc/upstream-netbsd/android/include/';
  exportSources(bionicSource,pins.aospBionic,[
    ...relativeUnits,
    ...headers,
    support+'netbsd-compat.h',
    support+'namespace.h',
    'libc/include/regex.h',
    'libc/include/ctype.h',
    'libc/include/malloc.h',
    'libc/Android.bp'
  ],source);
  const include=path.join(output,'include');
  writeChanged(path.join(include,'regex.h'),fs.readFileSync(path.join(source,'libc/include/regex.h')));
  const annotations=path.join(output,'annotations.h');
  const ctype=fs.readFileSync(path.join(source,'libc/include/ctype.h'),'utf8');
  const characters=[...new Map([...ctype.matchAll(/__BIONIC_CTYPE_INLINE\s+int\s+(\w+)\s*\(([^)]*)\)/g)].map(match=>[match[1],match[2]]))].filter(([name])=>!name.endsWith('_l'));
  // Use the original Android overflow-safe fallback and the existing physical
  // realloc owner; Darwin's newer reallocarray export raises the macOS floor.
  const allocation=fs.readFileSync(path.join(source,'libc/include/malloc.h'),'utf8').match(/static __inline __nodiscard void\* _Nullable reallocarray\([\s\S]*?\n\}/)?.[0];
  if(!allocation)throw Error('Original Android reallocarray fallback is absent');
  writeChanged(annotations,'#include <sys/cdefs.h>\n#include <stdlib.h>\n#include <ctype.h>\n#include <errno.h>\n#undef __RCSID\n#define __RCSID(x)\n#define __LIBC_HIDDEN__ __attribute__((visibility("hidden")))\n#define __BIONIC_COMPLICATED_NULLNESS _Nullable\n#define __nodiscard\n#define __BIONIC_ALLOC_SIZE(x,y)\n#define reallocarray klj_regex_reallocarray\n'+allocation+'\n'+characters.map(([name,parameters])=>'#undef '+name+'\nextern int klj_ctype_'+name+'('+parameters+');\n#define '+name+' klj_ctype_'+name).join('\n')+'\nextern size_t klb_ctype_mb_cur_max(void);\n#undef MB_CUR_MAX\n#define MB_CUR_MAX klb_ctype_mb_cur_max()\n#include '+JSON.stringify(path.join(source,support,'netbsd-compat.h'))+'\n');
  return {source,include,annotations,names,relativeUnits,headers,units:relativeUnits.map(file=>path.join(source,file)),support:path.join(source,support)};
}

export function prepareNativeRegex({bionicSource,build,ninja,python}){
  const output=path.join(path.resolve(build),'native-regex'),prepared=prepareRegexSources({bionicSource,output});
  const namespace=path.join(output,'namespace.h');
  writeChanged(namespace,[...prepared.names,'__regex_wctype','__regex_iswctype'].map(name=>'#define '+name+' klj_regex_'+name).join('\n')+'\n');
  const flags=['-arch','arm64','-mmacosx-version-min=13.0','-O2','-fPIC','-fno-builtin','-funsigned-char','-DPOSIX_MISTAKE','-Wno-unused-parameter','-Wno-sign-compare','-I'+prepared.include,'-I'+prepared.support,'-include',prepared.annotations,'-include',namespace];
  const compile=(name,file)=>compileHostObject({name,inputs:[file],flags,directory:path.join(output,'objects'),cwd:prepared.source,ninja,python}).output;
  const objects=prepared.units.map(file=>compile(path.basename(file)+'.o',file));
  const lookup=path.join(output,'lookup.c');
  writeChanged(lookup,'#include <regex.h>\n#include <string.h>\nvoid *klj_regex_lookup(const char *name){\n'+prepared.names.map(name=>' if(!strcmp(name,"'+name+'"))return (void *)&'+name+';').join('\n')+'\n return NULL;\n}\n');
  objects.push(compile('lookup.o',lookup));

  return {
    ...prepared,
    output,
    namespace,
    objects
  };
}
