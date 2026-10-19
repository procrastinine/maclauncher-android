import fs from 'node:fs';
import path from 'node:path';
import {exportSources} from '../binder-rpc/build.mjs';
import {writeChanged} from '../upstream/source-files.mjs';

const pins=JSON.parse(fs.readFileSync(new URL('../source-pins.json',import.meta.url)));
function body(text,signature){
  const start=text.indexOf(signature),open=text.indexOf('{',start);if(start<0||open<0)throw Error('Original printf argument planner changed');
  let end=open+1,depth=1;for(;depth&&end<text.length;end++){if(text[end]==='{')depth++;if(text[end]==='}')depth--;}
  return text.slice(start,end);
}

// Preserve Bionic's complete positional/length/width parser. Only its argument
// fetch is replaced with a type callback; the shared native formatter still
// owns formatting, streams, locale, diagnostics and output.
export function prepareFormatPlan({bionicSource,output}){
  const source=path.join(output,'format-source');
  exportSources(bionicSource,pins.aospBionic,['libc/stdio/printf_common.h','libc/stdio/scanf_common.h','libc/stdio/vfscanf.cpp','libc/NOTICE'],source);
  const original=fs.readFileSync(path.join(source,'libc/stdio/printf_common.h'),'utf8');
  let find=body(original,'static int __find_arguments(const CHAR_TYPE* fmt0, va_list ap, union arg** argtable,\n');
  const declarations=[...original.matchAll(/^#define (?:T\w*|\w*INT|ALT|LADJUST|LONGDBL|ZEROPAD|FPT|DEFPREC|STATIC_ARG_TBL_SIZE)\b[^\n]*/gm)].map(m=>m[0]).join('\n');
  const digits=original.match(/#define APPEND_DIGIT[\s\S]*?while \(0\)/)?.[0];
  if(!digits)throw Error('Original printf overflow parser is unavailable');
  const types=[];
  const reader=find.slice(find.indexOf('  for (n = 1; n <= tablemax; n++)'));
  const cases=[...reader.matchAll(/((?:\s*case \w+:)+)\s*\(\*argtable\)\[n\]\.\w+ = va_arg\(ap, ([^)]+)\);\s*break;/g)].map(match=>{
    let index=types.indexOf(match[2]);if(index<0){index=types.length;types.push(match[2]);}
    return match[1]+` if(consume(context,${index})!=0){ret=-1;goto finish;}break;`;
  });
  if(!cases.length||!cases.some(x=>x.includes('T_LONG_DOUBLE')))throw Error('Original printf argument fetches were not found');
  find=find.replace(/static int __find_arguments\([\s\S]*?\) \{/, 'static int plan(const CHAR_TYPE* fmt0,void* context,int(*consume)(void*,unsigned)) {');
  const done=find.indexOf('done:'),overflow=find.indexOf('\noverflow:',done);
  find=find.slice(0,done)+'done:\n  for(n=1;n<=tablemax;n++){switch(typetable[n]){'+cases.join('\n')+'\n}}\n  goto finish;\n'+find.slice(overflow);
  find=find.replace('munmap(typetable, *argtablesiz)','munmap(typetable, tablesize)').replaceAll('#if defined(__LP64__)','#if 0 /* original ARM32 target */');
  const grow=body(original,'static int __grow_type_table(unsigned char** typetable, int* tablesize) {');
  const common='#include <sys/mman.h>\n#include <sys/types.h>\n#include <unistd.h>\n#include <limits.h>\n#include <errno.h>\n#include <string.h>\n#include <stdint.h>\n#include <wchar.h>\n#include <stdexcept>\n#include "format-plan.h"\n#define __BIONIC_FALLTHROUGH [[fallthrough]]\n#define __fortify_fatal(message,...) throw std::invalid_argument(message)\n'+declarations+'\n#define to_digit(c) ((c)-\'0\')\n#define is_digit(c) ((unsigned)to_digit(c)<=9)\n'+digits+'\n';
  const implementation=['char','wchar_t'].map((type,index)=>`namespace plan${index}{\n#define CHAR_TYPE ${type}\nstatic int __grow_type_table(unsigned char**,int*);\n${find}\n${grow}\n#undef CHAR_TYPE\n}\nextern "C" int klj_arm32_format_plan${index}(const ${type}*fmt,void*context,int(*consume)(void*,unsigned)){return plan${index}::plan(fmt,context,consume);}`).join('\n');
  const scan=prepareScanPlan(source);
  writeChanged(path.join(output,'format-plan.cpp'),common+implementation+'\n'+scan.source);
  writeChanged(path.join(output,'format-plan.h'),'#pragma once\n#include <wchar.h>\nextern "C" {\nint klj_arm32_format_plan0(const char*,void*,int(*)(void*,unsigned));\nint klj_arm32_format_plan1(const wchar_t*,void*,int(*)(void*,unsigned));\nint klj_arm32_scan_plan(const char*,void*,int(*)(void*,unsigned));\n}\n');
  const header=path.join(output,'format-types.h');
  writeChanged(header,'#include <stddef.h>\n#include <stdint.h>\n#include <inttypes.h>\n#include <sys/types.h>\n#include <wchar.h>\nvoid __maclauncher_format_types('+types.map((type,index)=>type+' arg'+index).join(',')+');\nvoid __maclauncher_scan_types('+scan.types.map((type,index)=>type+' arg'+index).join(',')+');\n');
  const notices=path.join(output,'notices');fs.mkdirSync(notices,{recursive:true});fs.copyFileSync(path.join(source,'libc/NOTICE'),path.join(notices,'format-Bionic-NOTICE'));
  return {header,types,source:path.join(output,'format-plan.cpp')};
}

/** Reuse vfscanf's complete conversion-specifier switch, scanset parser and
 * integer type/length selection. Only its input operations and typed va_arg
 * stores become an argument-layout callback; the shared native scanner still
 * owns actual input, matching, locale and assignments. */
function prepareScanPlan(source){
 const original=fs.readFileSync(path.join(source,'libc/stdio/vfscanf.cpp'),'utf8'),common=fs.readFileSync(path.join(source,'libc/stdio/scanf_common.h'),'utf8'),types=[];
 const id=type=>{let n=types.indexOf(type);if(n<0){n=types.length;types.push(type);}return n;};
 const callbacks=text=>text.replace(/\*va_arg\(ap, ([^)]+)\) = [^;]+;/g,(_,type)=>`if(consume(context,${id(type)})!=0)return -1;`);
 let selection=original.slice(original.indexOf('again:'),original.indexOf('    if ((flags & ALLOCATE)'));
 selection=selection.replace(/case '%':[\s\S]*?(?=      case '\*':)/,"case '%': continue;\n");
 selection=callbacks(selection).replace('return EOF;','return -1;');
 const integerStart=original.indexOf('          if (flags & POINTER) {'),integerEnd=original.indexOf('          nassigned++;',integerStart);
 if(integerStart<0||integerEnd<0)throw Error('Original scanf integer argument selection changed');
 const integer=callbacks(original.slice(integerStart,integerEnd));
 const floatStart=original.indexOf('          if (flags & LONGDBL) {',original.lastIndexOf('      case CT_FLOAT:')),floatEnd=original.indexOf('          if (static_cast<size_t>',floatStart);
 if(floatStart<0||floatEnd<0)throw Error('Original scanf floating-point argument selection changed');
 const floating=callbacks(original.slice(floatStart,floatEnd).replace(/(?:long double|double|float) res =[^;]+;/g,''));
 const character=`if(flags&LONG){if(flags&ALLOCATE){if(consume(context,${id('wchar_t**')})!=0)return -1;}else if(consume(context,${id('wchar_t*')})!=0)return -1;}else{if(flags&ALLOCATE){if(consume(context,${id('char**')})!=0)return -1;}else if(consume(context,${id('char*')})!=0)return -1;}`;
 const defines=[...common.matchAll(/^#define (\w+)[^\n]*/gm)].filter(m=>!['to_digit','is_digit','APPEND_DIGIT'].includes(m[1]));
 const digits=common.match(/#define APPEND_DIGIT[\s\S]*?while \(0\)/)?.[0];if(!digits)throw Error('Original scanf width parser changed');
 const macros=defines.map(m=>'#undef '+m[1]+'\n'+m[0]).join('\n')+'\n#undef APPEND_DIGIT\n'+digits;
 const width=body(common,'static int w_to_flag(int size, bool fast) {').replace('sizeof(void*) == 8','false');
 const scanset=body(original,'static const unsigned char* __sccl(char* tab, const unsigned char* fmt) {');
 return {types,source:'#include <ctype.h>\n#include <stdio.h>\n#include <inttypes.h>\n'+macros+'\nnamespace scan_plan {\n'+width+'\n'+scanset+'\nstatic int plan(const char *fmt0,void *context,int(*consume)(void*,unsigned)){const unsigned char *fmt=reinterpret_cast<const unsigned char *>(fmt0);int c,flags;[[maybe_unused]]int base;[[maybe_unused]]size_t width;char ccltab[256];for(;;){c=*fmt++;if(!c)return 0;if(c!=\'%\')continue;width=0;flags=0;\n'+selection+'\nif(flags&SUPPRESS)continue;if(c<=CT_STRING){'+character+'}else if(c==CT_INT){'+integer+'}else if(c==CT_FLOAT){'+floating+'}\n}}\n}\nextern "C" int klj_arm32_scan_plan(const char *format,void *context,int(*consume)(void*,unsigned)){return scan_plan::plan(format,context,consume);}\n'};
}
