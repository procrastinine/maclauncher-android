import fs from 'node:fs';
import path from 'node:path';
import {writeChanged} from './upstream/source-files.mjs';

// The existing callback contract remains the single Java method/descriptor
// supplier for both VM bindings. No framework class is replaced or selected.
export function prepareWebkitCallbacks(output){
 const rows=fs.readFileSync(path.join(import.meta.dirname,'host-callbacks.tsv'),'utf8').split(/\r?\n/).filter(line=>line&&!line.startsWith('#')).map(line=>line.split('\t')).filter(row=>row[0].startsWith('web_')||row[0]==='failure_report');
 fs.mkdirSync(output,{recursive:true});
 const file=path.join(output,'webkit-callbacks.inc');
 writeChanged(file,rows.map(([key,owner,name,descriptor,invocation])=>`{${[key,owner,name,descriptor].map(JSON.stringify).join(',')},${invocation==='static'}},`).join('\n')+'\n');
 return output;
}
