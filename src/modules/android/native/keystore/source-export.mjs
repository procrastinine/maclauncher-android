import {addLicense} from '../upstream/licenses.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {execFileSync} from 'node:child_process';
import {exportSources} from '../binder-rpc/build.mjs';
import {prepareSourceTree} from '../upstream/source-files.mjs';

const catalog=JSON.parse(fs.readFileSync(new URL('./sources.json',import.meta.url)));
const nativeRoot=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');

/** Export pinned input trees before applying host adaptations in the build area. */
export function prepareKeyStoreSources({checkouts={},output}) {
  const sources={};
  const licenses=path.join(output,'licenses');

  for(const record of catalog){
    const checkout=checkouts[record.name]||path.resolve(nativeRoot,'../../../external',record.checkout);
    const destination=path.join(output,'source',record.name);
    const trees=record.filePatterns
      ?execFileSync('git',['-C',checkout,'ls-tree','-r','--name-only',record.revision,...record.trees],{encoding:'utf8',maxBuffer:32<<20})
        .trim().split('\n').filter(file=>record.filePatterns.some(pattern=>path.matchesGlob(file,pattern)))
      :record.trees;
    prepareSourceTree(destination,temporary=>exportSources(checkout,record.revision,trees,temporary));
    sources[record.name]=destination;
    // Preserve licenses supplied with the exact exported source closure.
    for(const relative of fs.globSync(['**/NOTICE','**/NOTICE.*','**/LICENSE','**/LICENSE.*','**/COPYING','**/COPYING.*'],{cwd:destination})){
      const file=path.join(destination,relative);
      addLicense(output,record.name,file);
    }
  }
  return {sources,licenses,catalog};
}
