/** OpenSL/OpenMAX declare the same counted interface contract on exported
 * functions and interface-table methods: numInterfaces, pInterfaceIds and
 * pInterfaceRequired. Clang retains the original typedef identities even when
 * function-pointer parameter names are absent from the canonical type. */
export function mediaInterfaceCounts(signature,declarations){
 const types=declarations.types,parameters=signature.parameters||[],plans=[];
 const alias=(type,name)=>type?.aliases?.includes(name);
 for(let index=1;index+1<parameters.length;index++){
  const pointer=types[parameters[index]],identifier=types[pointer?.element];
  if(pointer?.kind!=='Pointer'||identifier?.kind!=='Pointer'||!identifier.const)continue;
  const family=['SL','XA'].find(prefix=>alias(identifier,prefix+'InterfaceID'));
  if(!family)continue;
  const count=types[parameters[index-1]],required=types[parameters[index+1]],boolean=types[required?.element];
  if(!alias(count,family+'uint32')||required?.kind!=='Pointer'||!boolean?.const||!alias(boolean,family+'boolean'))
   throw Error('Original media counted-interface declaration changed: '+signature.name);
  for(const parameter of [index,index+1])plans.push({parameter,instructions:[{operation:'Parameter',value:index-1}]});
 }
 return plans;
}
