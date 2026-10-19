import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';

/** Developer-build transport to the pinned upstream Blueprint parser. Original
 * Soong owns grammar and expression semantics; this adapter only carries JSON. */
export function parseBlueprints(documents,{parser=process.env.ANDROID_BLUEPRINT_PARSER}={}) {
  if(!parser)throw Error('The native build requires its pinned original Blueprint parser input');
  const output=execFileSync(parser,[],{input:JSON.stringify(documents),encoding:'utf8',maxBuffer:128*1024*1024});
  const parsed=JSON.parse(output);
  if(!Array.isArray(parsed)||parsed.length!==documents.length)throw Error('Blueprint parser returned an invalid document result');
  return parsed;
}

export function parseBlueprint(text,{file='<Blueprint>',parser,parents=[]}={}) {
  const [result]=parseBlueprints([{file,source:text,parents}],{parser});
  if(result.error)throw new SyntaxError(result.error);
  const {modules,assignments,variables}=result;
  return {modules,assignments,variables};
}

/** Return a named declaration's own list; variant selection belongs to Soong. */
export function readModuleList(directory,module,property,options={}){
  const file=path.join(directory,'Android.bp');
  const matches=parseBlueprint(fs.readFileSync(file,'utf8'),{file,...options}).modules.filter(value=>value.properties.name===module);
  if(matches.length!==1)throw Error(`${file}: expected one module named ${module}, found ${matches.length}`);
  const value=matches[0].properties[property];
  if(!Array.isArray(value)||!value.every(item=>typeof item==='string'))throw Error(`${file}: ${module}.${property} must be a resolved string array`);
  return [...value];
}
