import path from 'node:path';
/** Retire the competing bounded bag; all NDK symbols come from the original
 * media library and the existing Mac decoder consumes that same format owner. */
export function mediaFormatSource(source){
  const begin=source.indexOf('// AMediaFormat — a key/value bag'),end=source.indexOf('// The format-key constants.',begin);
  if(begin<0||end<begin)throw Error('Pinned native format implementation changed');
  source=source.slice(0,begin)+'#include '+JSON.stringify(path.join(import.meta.dirname,'media-format.h'))+'\n'+source.slice(end);
  const constants=[...source.matchAll(/^static const char \*(g_key_\w+)\s*=\s*("[^"]*");$/gm)];
  source=source.replace(/^static const char \*g_key_\w+\s*=\s*"[^"]*";\n/gm,'');
  // This table contains both functions and pointer-valued data exports. Resolve
  // their complete original library instead of retaining selected table names.
  source=source.replace(/^\s*M\("(?:AMediaFormat_|AMEDIAFORMAT_)[^\n]+\n/gm,'');
  for(const [name,member]of Object.entries({new:'create',setInt32:'setInt32',setInt64:'setInt64',setString:'setString'}))source=source.replaceAll('klm_AMediaFormat_'+name,'klj_media_formats()->'+member);
  source=source.replaceAll('klm_format','AMediaFormat').replaceAll('klm_fmt_string','klj_media_format_string');
  for(const [,name,value]of constants)source=source.replace(new RegExp('\\b'+name+'\\b','g'),value);
  source=source.replace('klj_media_formats()->setInt32(f, "durationUs", (int32_t)dur);','klj_media_formats()->setInt64(f, "durationUs", dur);');
  const lookup='void *kl_mediandk_lookup(const char *name) {';
  if(source.split(lookup).length!==2)throw Error('Pinned native media lookup changed');
  return source.replace(lookup,lookup+'\n    if(name&&(!strncmp(name,"AMediaFormat_",13)||!strncmp(name,"AMEDIAFORMAT_",13)))return klj_media_format_lookup(name);');
}
