import fs from 'node:fs';

/** A complete native owner replaces the old sample forwarders as a unit.
 * Only generated build copies are changed; the pinned supplier stays intact. */
export function claimNativeFamily({shim,table,names}){
  const claimed=new Set(names);
  fs.writeFileSync(table,fs.readFileSync(table,'utf8').replace(/^KL_FWD\((\w+)\).*\n/gm,(line,name)=>claimed.has(name)?'':line));
  fs.writeFileSync(shim,fs.readFileSync(shim,'utf8').replace(/E\("(\w+)",\s*\w+\),?/g,(entry,name)=>claimed.has(name)?'':entry));
}

/** Retire the pinned sample's zlib forwards. Complete original guest libz now
 * supplies these exports; this does not alter any libc or host compression call. */
export function retireCompressionForwarders(table){
  const original=fs.readFileSync(table,'utf8');
  const retired=/^KL_FWD\((?:adler32|crc32|compressBound|compress2|deflateBound|deflate|deflateEnd|deflateInit2_|deflateInit_|deflateReset|inflate|inflateEnd|inflateInit2_|inflateInit_|inflateReset|inflateReset2|uncompress|zlibVersion)\)\r?\n/gm;
  if([...original.matchAll(retired)].length!==18)throw Error('Pinned compression forwarders changed');
  fs.writeFileSync(table,original.replace(retired,''));
}

/** Original Bionic owns fork callbacks and vfork's cached-pid bookkeeping.
 * Only its kernel entry is adapted; Darwin public functions cannot bypass it. */
export function retireProcessForwarders(table){
  const original=fs.readFileSync(table,'utf8');
  fs.writeFileSync(table,original.replace(/^KL_FWD\((fork|vfork)\).*\n/gm,''));
}
