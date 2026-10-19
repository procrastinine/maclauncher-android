/** Extend the original header block after its first include. Translation-unit
 * configuration and the upstream license remain ahead of all dependencies. */
export function includeDependencies(source,declarations){
  const first=/^[\t ]*#[\t ]*include[\t ]+[<"][^\n]*\n/m.exec(source);
  if(!first)throw Error('Original native source header block is unavailable');
  const at=first.index+first[0].length;
  return source.slice(0,at)+declarations+'\n'+source.slice(at);
}
