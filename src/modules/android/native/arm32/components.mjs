import path from 'node:path';
import {writeChanged} from '../upstream/source-files.mjs';
import components from './components.json' with {type:'json'};
export {components};

/** One native-width family, consumed by both VM delivery and preload bindings. */
export function arm32BuildProducts(output){
 return {library:path.join(output,components.bindings.jvm),
  cpu:{archive:path.join(output,components.cpu.archive),dependencies:components.cpu.dependencies.map(file=>path.join(output,file))},
  preloads:Object.fromEntries(components.preloads.map(item=>[item.name,path.join(output,item.build)])),
  notices:path.join(output,components.notices)};
}
export function prepareArm32Components(output){
 const file=path.join(output,'arm32-components.h');
 writeChanged(file,'#pragma once\n#define KLJ_ARM32_JVM_LIBRARY '+JSON.stringify(components.bindings.jvm)+'\n'+
  'static const char *const klj_arm32_preloads[] = {'+components.preloads.map(item=>JSON.stringify(item.file)).join(',')+'};\n');
 return file;
}
