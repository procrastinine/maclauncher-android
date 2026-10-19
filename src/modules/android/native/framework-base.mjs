import fs from 'node:fs';
import path from 'node:path';
import {verifyBinaryInputs} from './binary-inputs.mjs';

/** Build inputs for the complete AOSP framework and owned platform adapters.
 * Architecture and source-inventory checks live exclusively in tests. */
export function frameworkInputs(root, directory) {
  const definition=JSON.parse(fs.readFileSync(path.join(root,'framework-binary.json')));
  const sources=[];
  function visit(directory){for(const item of fs.readdirSync(directory,{withFileTypes:true})){
    const file=path.join(directory,item.name);
    if(item.isDirectory())visit(file);else if(item.isFile()&&file.endsWith('.java'))sources.push(file);
  }}
  visit(path.join(root,'java'));
  if(fs.existsSync(path.join(root,'java-jvm')))visit(path.join(root,'java-jvm'));
  return {definition,sources,input:verifyBinaryInputs(directory,definition.files)[0]};
}
