import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';
import {cachedBuildStep} from '../upstream/build-step.mjs';
import {writeChanged} from '../upstream/source-files.mjs';
import {generateSignalAbi} from './signal-abi.mjs';
import {mediaInterfaceCounts} from './media-lengths.mjs';

const root=import.meta.dirname;
export function readGraphicsLengths({python,registry,notice}){
  return JSON.parse(execFileSync(python,[path.join(root,'registry-lengths.py'),registry,...(notice?[notice]:[])],{encoding:'utf8',maxBuffer:16*1024*1024}));
}
// Registry lengths describe element cardinality; the original Clang types
// supply each ABI's element layout. Unspecified dynamic GL state remains with
// the original graphics owner, without disabling a command or parameter.
export function compileGraphicsLength(expression,parameters,signature,declarations){
  if(!expression)return null;
  expression=expression.split(',null-terminated')[0].trim();
  const parameterIndex=name=>parameters.findIndex(parameter=>parameter.name===name||parameter.aliases?.includes(name));
  const component=/^COMPSIZE\(\s*(\w+)\s*\)$/.exec(expression);
  if(component){
    const index=parameterIndex(component[1]);
    const type=declarations.types[signature.parameters[index]];
    if(!type?.aliases?.some(alias=>alias==='GLsizei'||alias==='GLsizeiptr'))return null;
    expression=component[1];
  }
  const tokens=expression.match(/\w+|[()+*\-/]/g)||[];
  if(tokens.join('')!==expression.replace(/\s/g,''))return null;
  let offset=0;
  const atom=()=>{
    const token=tokens[offset++];
    if(token==='('){const value=sum();if(tokens[offset++]!==')')throw 0;return value;}
    if(/^\d+$/.test(token||''))return [{operation:'Constant',value:Number(token)}];
    const index=parameterIndex(token);
    const type=declarations.types[signature.parameters[index]];
    if(index<0||!type||!['Int','UInt','Long','ULong','LongLong','ULongLong','Short','UShort'].includes(type.kind))throw 0;
    return [{operation:'Parameter',value:index}];
  };
  const product=()=>{let left=atom();while(tokens[offset]==='*'||tokens[offset]==='/'){const op=tokens[offset++];left=[...left,...atom(),{operation:op==='*'?'Multiply':'Divide',value:0}];}return left;};
  const sum=()=>{let left=product();while(tokens[offset]==='+'||tokens[offset]==='-'){const op=tokens[offset++];left=[...left,...product(),{operation:op==='+'?'Add':'Subtract',value:0}];}return left;};
  try{const result=sum();return offset===tokens.length?result:null;}catch{return null;}
}
/** Evaluate the complete original dlfcn control family for both Android ABIs.
 * Pointer sentinels and mode bits differ on LP32; Clang owns their values. */
export function generateLoaderAbi({include,output,clang='clang'}){
 const header=fs.readFileSync(path.join(include,'dlfcn.h'),'utf8');
 const names=[...new Set([...header.matchAll(/^\s*#\s*define\s+(RTLD_\w+)\s+/gm)].map(match=>match[1]))].sort();
 const source='#include <stdint.h>\n#include <dlfcn.h>\n'+names.map(name=>`unsigned long long klj_${name}(void){return (uintptr_t)${name};}`).join('\n');
 const values={};
 for(const [key,target,arch] of [['guest','armv7a-linux-androideabi36','arm-linux-androideabi'],['host','aarch64-linux-android36','aarch64-linux-android']]){
  const ir=execFileSync(clang,['--target='+target,'-isystem',include,'-isystem',path.join(include,arch),'-x','c','-O2','-S','-emit-llvm','-o','-','-'],{input:source,encoding:'utf8',maxBuffer:1024*1024});
  values[key]=Object.fromEntries([...ir.matchAll(/define[^\n]*@klj_(RTLD_\w+)\([^]*?ret i64 (-?\d+)/g)].map(match=>[match[1],BigInt.asUintN(64,BigInt(match[2])).toString()]));
  for(const name of names)if(values[key][name]===undefined)throw Error('Original loader constant did not compile: '+name);
 }
 fs.mkdirSync(output,{recursive:true});
 writeChanged(path.join(output,'loader-abi.inc'),'// Original NDK dlfcn.h, evaluated by Clang for LP32 and LP64.\n'+names.map(name=>`static constexpr uint64_t guest_${name}=UINT64_C(${values.guest[name]}),host_${name}=UINT64_C(${values.host[name]});`).join('\n')+'\n'+
  'static constexpr struct {uint32_t guest,host;} loaderFlags[] = {'+names.filter(name=>!['RTLD_DEFAULT','RTLD_NEXT'].includes(name)).map(name=>`{uint32_t(guest_${name}),uint32_t(host_${name})}`).join(',')+'};\n');
 return values;
}
/** Khronos data-source/sink pointers carry a discriminator, not an opaque
 * resource. Recover every declared locator/format through the original naming
 * contract and evaluate its tag with Clang; no numeric ABI values are copied. */
export function readMediaTags({include,guest,host,clang='clang'}){
  const rows=[];
  for(const [name,type] of Object.entries(guest.types)){
    const match=/^(SL|XA)(Android)?Data(Locator|Format)_(.+)$/.exec(name);
    if(!match||type.kind!=='Record')continue;
    const first=type.fields?.[0],other=host.types[name]?.fields?.[0];
    if(first?.name!==(match[3]==='Locator'?'locatorType':'formatType')||first.offset!==0||other?.name!==first.name||other.offset!==0)throw Error('Original media discriminator layout changed: '+name);
    const tag=[match[1],...(match[2]?['ANDROID']:[]),'DATA'+match[3].toUpperCase(),match[4].toUpperCase()].join('_');
    rows.push({name,tag,family:(match[1]==='SL'?0:2)+(match[3]==='Locator'?1:2),header:type.declaration?.file.replace('<ndk>/','')});
  }
  if(!rows.length)return [];
  const headers=[...new Set(rows.map(row=>row.header))];
  if(headers.some(header=>!header||path.isAbsolute(header)))throw Error('Original media type has no NDK declaration');
  const source=headers.map(header=>'#include <'+header+'>').join('\n')+'\n'+rows.map((row,index)=>`unsigned long long klj_tag_${index}(void){return ${row.tag};}`).join('\n');
  for(const [key,target,arch] of [['guest','armv7a-linux-androideabi36','arm-linux-androideabi'],['host','aarch64-linux-android36','aarch64-linux-android']]){
    const ir=execFileSync(clang,['--target='+target,'-isystem',include,'-isystem',path.join(include,arch),'-x','c','-O2','-S','-emit-llvm','-o','-','-'],{input:source,encoding:'utf8',maxBuffer:1024*1024});
    for(const match of ir.matchAll(/define[^\n]*@klj_tag_(\d+)\([^]*?ret i64 (\d+)/g))rows[Number(match[1])][key]=Number(match[2]);
    for(const row of rows)if(!Number.isSafeInteger(row[key]))throw Error('Original media tag did not compile: '+row.tag);
  }
  return rows;
}
export function emitDescriptors({guest,host,output,graphicsLengths={},mediaTags=[]}){
  const allocationAlignment=host.types.max_align_t?.alignment;
  if(host.functions.malloc&&(!Number.isSafeInteger(allocationAlignment)||allocationAlignment<1||(allocationAlignment&(allocationAlignment-1))))throw Error('Original native allocation alignment is unavailable');
  const types=[],pairs=new Map(),typedCounts=[],q=JSON.stringify;
  const kind=t=>!t?'Unsupported':t.kind==='Void'?'Void':t.kind==='Pointer'?'Pointer':t.kind==='Record'?'Record':t.kind.includes('Array')?'Array':t.kind.startsWith('Function')?'Function':['Float','Double','LongDouble'].includes(t.kind)?'Float':['Bool','Char_U','UChar','UShort','UInt','ULong','ULongLong','UInt128'].includes(t.kind)?'Unsigned':['Char_S','SChar','Short','Int','Long','LongLong','Int128'].includes(t.kind)?'Signed':'Unsupported';
  const jniValue=(t,declarations)=>{
    if(t?.kind!=='Pointer')return 'None';
    if(t.aliases?.includes('jobject'))return 'Object';
    if(t.aliases?.includes('jmethodID'))return 'Method';
    if(t.aliases?.includes('jfieldID'))return 'Field';
    const pointee=declarations.types[t.element];
    if(pointee?.aliases?.includes('JNIEnv'))return 'Environment';
    if(pointee?.aliases?.includes('JavaVM'))return 'VM';
    return 'None';
  };
  // EGL's core/extension void-pointer typedefs are opaque API values, including
  // application labels. They are not byte buffers or platform window pointers.
  const eglValue=(type,declarations)=>type?.kind==='Pointer'&&
    declarations.types[type.element]?.kind==='Void'&&
    /^<ndk>\/EGL\/(?:egl|eglext)\.h$/.test(type.declaration?.file||'')&&
    type.aliases?.some(name=>name.startsWith('EGL'));
  function pair(gname,hname){
    let g=guest.types[gname],h=host.types[hname];
    if(g?.kind==='Enum')return pair(g.element,h?.kind==='Enum'?h.element:hname);
    if(h?.kind==='Enum')return pair(gname,h.element);
    const key=JSON.stringify([gname,hname]);if(pairs.has(key))return pairs.get(key);
    const id=types.length,t={name:gname||hname||'unknown',kind:kind(g),hostKind:kind(h),gs:g?.size||0,hs:h?.size||0,ga:g?.alignment||1,ha:h?.alignment||1,constant:!!g?.const,union:!!g?.union,hostUnion:!!h?.union,element:0,result:0,params:[],fields:[],hostFields:[],count:0,hostCount:Math.max(0,h?.count||0),variadic:!!g?.variadic,jni:jniValue(g,guest),packedBits:false,eglValue:!!(eglValue(g,guest)&&eglValue(h,host))};
    pairs.set(key,id);types.push(t);
    if(kind(h)==='Record')t.hostFields=(h.fields||[]).map(f=>({type:pair(undefined,f.type),bits:f.bits}));
    if(!g&&h?.element){t.element=pair(undefined,h.element);t.count=Math.max(0,h.count||0);}
    // Bionic represents the same signal bitmap as an unsigned word on ARM32
    // and a one-word record on ARM64 (the 64-bit API uses two ARM32 words).
    // Its bits cross the ABI boundary; signal policy remains in the common
    // native owner. Preserve all original declarations and pointer constness.
    const signalSet=g?.aliases?.some(name=>name==='sigset_t'||name==='sigset64_t')&&h?.aliases?.some(name=>name==='sigset_t'||name==='sigset64_t');
    // fd_set is one fixed-width bitmap, not an index-preserving array of
    // fd_mask integers. Both little-endian Android ABIs store the same bits,
    // using 32 words on ARM32 and 16 words on ARM64 for the original 1024 bits.
    t.packedBits=!!(g?.aliases?.includes('fd_set')&&h?.aliases?.includes('fd_set')&&g.size===h.size);
    const integers=['Signed','Unsigned'];
    if(signalSet)t.kind=t.hostKind='Unsigned';
    // Original typedefs can change signedness as well as width between Android
    // ABIs. Preserve each side for native FFI and directional integer extension.
    else if(!g||!h||kind(g)!==kind(h)&&!(integers.includes(kind(g))&&integers.includes(kind(h))))t.kind='Unsupported';
    else if(['Pointer','Array'].includes(t.kind)&&(!g.element||!h.element))t.kind='Unsupported';
    else if(g.element){t.element=pair(g.element,h.element);t.count=Math.max(0,g.count||0);}
    else if(t.kind==='Function'){
      t.result=pair(g.result,h.result);t.params=g.parameters.map((name,n)=>pair(name,h.parameters[n]));
      const lengths=mediaInterfaceCounts(g,guest);
      if(lengths.length){
        if(JSON.stringify(lengths)!==JSON.stringify(mediaInterfaceCounts(h,host)))throw Error('Original media interface cardinality differs between targets: '+gname);
        typedCounts.push({type:id,lengths});
      }
    }else if(t.kind==='Record'){
      // Native libffi layout follows the native declaration, independently of
      // conversion edges. Field order, padding and legacy ABI mirrors can all
      // differ without changing the native function's actual record layout.
      t.fields=(g.fields||[]).map(f=>{
        let other=h.fields?.find(x=>x.name===f.name),role='Value';
        // Bionic's ARM32 stat ABI retains a low-word inode mirror alongside
        // st_ino. The canonical field supplies it on output; it never replaces
        // the full-width inode on input. Both stat/stat64 use this declaration.
        if(!other&&f.name==='__st_ino'&&g.declaration?.file.endsWith('/sys/stat.h')&&g.fields.some(x=>x.name==='st_ino')){
          other=h.fields?.find(x=>x.name==='st_ino');if(other)role='Mirror';
        }
        // Reserved storage is architecture-specific, not a missing logical
        // field. Retain its guest extent without pairing it to unrelated
        // native padding at the same ordinal position.
        if(!other&&/^__(?:pad|unused|reserved)\w*$/.test(f.name))role='Padding';
        const media=/^(?:struct )?(SL|XA)Data(?:Source|Sink)_?$/.exec(gname);
        const tagFamily=media&&['pLocator','pFormat'].includes(f.name)?(media[1]==='SL'?0:2)+(f.name==='pLocator'?1:2):0;
        return {name:f.name,type:pair(f.type,other?.type),go:f.offset,ho:other?.offset??-1,gb:f.bits,hb:other?.bits??0,role,tagFamily};
      });
    }
    return id;
  }
  const functions=Object.values(guest.functions).filter(g=>!['__maclauncher_format_types','__maclauncher_scan_types','__maclauncher_character_types','__maclauncher_file_types'].includes(g.name||g.symbol)).sort((a,b)=>a.symbol.localeCompare(b.symbol)).map(g=>{
    const parameters=graphicsLengths[g.symbol]||[],signature=guest.types[g.type];
    const lengths=parameters.flatMap((parameter,index)=>{
      if(guest.types[signature.parameters[index]]?.kind!=='Pointer')return [];
      const instructions=compileGraphicsLength(parameter.length,parameters,signature,guest);
      return instructions?[{parameter:index,instructions}]:[];
    });
    return {name:g.symbol,type:pair(g.type,host.functions[g.symbol]?.type),params:g.parameters,format:g.format,lengths};
  });
  const formatGuest=guest.types[guest.functions.__maclauncher_format_types?.type],formatHost=host.types[host.functions.__maclauncher_format_types?.type];
  const formatTypes=(formatGuest?.parameters||[]).map((type,index)=>pair(type,formatHost?.parameters[index]));
  const scanGuest=guest.types[guest.functions.__maclauncher_scan_types?.type],scanHost=host.types[host.functions.__maclauncher_scan_types?.type];
  const scanTypes=(scanGuest?.parameters||[]).map((type,index)=>pair(type,scanHost?.parameters[index]));
  const fileGuest=guest.types[guest.functions.__maclauncher_file_types?.type],fileHost=host.types[host.functions.__maclauncher_file_types?.type];
  const fileTypes=(fileGuest?.parameters||[]).map((type,index)=>pair(type,fileHost?.parameters[index]));
  const commandPrefix='__maclauncher_file_command_';
  const fileCommands=Object.values(guest.variables||{}).filter(g=>g.symbol.startsWith(commandPrefix)).map(g=>({name:g.symbol.slice(commandPrefix.length),guest:guest.types[g.type].count-1,host:host.types[host.variables?.[g.symbol]?.type]?.count-1})).filter(row=>Number.isFinite(row.host));
  const variables=Object.values(guest.variables||{}).filter(g=>!g.symbol.startsWith(commandPrefix)).sort((a,b)=>a.symbol.localeCompare(b.symbol)).map(g=>({name:g.symbol,type:pair(g.type,host.variables?.[g.symbol]?.type)}));
  // OpenSL/OpenMAX interface queries erase their output type to void*. The
  // original IID and interface typedef naming contract recovers that type;
  // every interface still calls the same native implementation and resources.
  const interfaces=[],interfaceQueries=[],queryTypes=new Set();
  const aliases=new Map(),declaredInterfaces=[];
  for(const [name,type] of Object.entries(guest.types))for(const alias of new Set([name,...type.aliases||[]])){
    const match=/^(SL|XA)(.+)Itf$/.exec(alias);
    if(match&&type.kind==='Pointer'){
      aliases.set(match[1]+'_'+match[2].replaceAll('_','').toUpperCase(),name);
      if(alias===name&&type.declaration)declaredInterfaces.push({name,family:match[1],...type.declaration});
    }
  }
  for(const variable of Object.values(guest.variables||{})){
    const match=/^(SL|XA)_IID_(.+)$/.exec(variable.symbol);if(!match)continue;
    const declaration=variable.declaration;
    // The standard headers declare each IID immediately before its interface
    // group. Preserve that original association too (LEDArray, ConfigExtensions
    // and BufferQueue do not mechanically spell their IID suffix).
    const next=declaration?Object.values(guest.variables||{}).filter(item=>/^(SL|XA)_IID_/.test(item.symbol)&&item.declaration?.file===declaration.file&&item.declaration.offset>declaration.offset).reduce((end,item)=>Math.min(end,item.declaration.offset),Infinity):0;
    const declared=declaration?declaredInterfaces.filter(item=>item.family===match[1]&&item.file===declaration.file&&item.offset>declaration.offset&&item.offset<next):[];
    const name=declared.length===1?declared[0].name:aliases.get(match[1]+'_'+match[2].replaceAll('_',''));if(!name)continue;
    const valueType=pair(name,name),identifierType=pair(variable.type,host.variables?.[variable.symbol]?.type);
    interfaces.push({name:variable.symbol,family:match[1]==='SL'?0:1,type:valueType,bytes:types[types[identifierType].element].hs});
    let record=guest.types[name];while(record?.kind==='Pointer')record=guest.types[record.element];
    for(const field of record?.fields||[]){
      if(field.name!=='GetInterface')continue;
      const pointer=pair(field.type,field.type),type=types[pointer].element,signature=types[type];
      if(signature?.kind==='Function'&&signature.params.length===3&&!queryTypes.has(type)){
        queryTypes.add(type);interfaceQueries.push({type,family:match[1]==='SL'?0:1,identifier:1,output:2});
      }
    }
  }
  const taggedRecords=mediaTags.map(row=>{
    const element=pair(row.name,row.name),pointer=pair('void *','void *'),id=types.length;
    // All data pointers use Clang's pointer ABI. Only the erased pointee is
    // recovered here; its fields/extent still come from the original record.
    types.push({...types[pointer],name:row.name+' *',element});
    return {...row,type:id};
  });
  const lines=['// Generated from original NDK declarations with Clang for both Android ABIs.'];
  types.forEach((t,i)=>{
    if(t.params.length)lines.push(`static constexpr unsigned params${i}[]={${t.params.join(',')}};`);
    if(t.fields.length)lines.push(`static constexpr Field fields${i}[]={${t.fields.map(f=>`{${f.type},${f.go},${f.ho},${f.gb},${f.hb},${q(f.name)},FieldRole::${f.role},${f.tagFamily}}`).join(',')}};`);
    if(t.hostFields.length)lines.push(`static constexpr NativeField hostFields${i}[]={${t.hostFields.map(f=>`{${f.type},${f.bits}}`).join(',')}};`);
  });
  lines.push('static constexpr Type types[]={');
  types.forEach((t,i)=>lines.push(`{Kind::${t.kind},${t.gs},${t.hs},${t.ga},${t.ha},${t.constant},${t.union},${t.element},${t.result},${t.params.length?'params'+i:'nullptr'},${t.params.length},${t.fields.length?'fields'+i:'nullptr'},${t.fields.length},${t.count},${t.variadic},${q(t.name)},JniValue::${t.jni},Kind::${t.hostKind},${t.hostFields.length?'hostFields'+i:'nullptr'},${t.hostFields.length},${t.hostCount},${t.hostUnion},${t.packedBits},${t.eglValue}},`));
  lines.push('};');
  functions.forEach((f,i)=>{
    if(f.params.length)lines.push(`static constexpr const char* names${i}[]={${f.params.map(q).join(',')}};`);
    f.lengths.forEach((length,j)=>lines.push(`static constexpr CountInstruction countInstructions${i}_${j}[]={${length.instructions.map(instruction=>`{CountOperation::${instruction.operation},${instruction.value}}`).join(',')}};`));
    if(f.lengths.length)lines.push(`static constexpr CountPlan counts${i}[]={${f.lengths.map((length,j)=>`{${length.parameter},countInstructions${i}_${j},${length.instructions.length}}`).join(',')}};`);
  });
  for(const row of typedCounts){
    row.lengths.forEach((length,index)=>lines.push(`static constexpr CountInstruction mediaInstructions${row.type}_${index}[]={${length.instructions.map(instruction=>`{CountOperation::${instruction.operation},${instruction.value}}`).join(',')}};`));
    lines.push(`static constexpr CountPlan mediaCounts${row.type}[]={${row.lengths.map((length,index)=>`{${length.parameter},mediaInstructions${row.type}_${index},${length.instructions.length}}`).join(',')}};`);
  }
  lines.push('static constexpr struct {unsigned type;const CountPlan *counts;unsigned size;} typedCounts[]={'+typedCounts.map(row=>`{${row.type},mediaCounts${row.type},${row.lengths.length}}`).join(',')+'};');
  lines.push('static constexpr Function functions[]={');functions.forEach((f,i)=>lines.push(`{${q(f.name)},${f.type},${f.params.length?'names'+i:'nullptr'},${f.format?.index??-1},${f.format?.first??-1},${f.format?.kind==='scanf'},${f.lengths.length?'counts'+i:'nullptr'},${f.lengths.length}},`));lines.push('};');
  lines.push(`static constexpr unsigned formatTypes[]={${formatTypes.length?formatTypes.join(','):'0'}};`);
  lines.push(`static constexpr unsigned scanTypes[]={${scanTypes.length?scanTypes.join(','):'0'}};`);
  lines.push(`static constexpr unsigned fileTypes[]={${fileTypes.length?fileTypes.join(','):'0'}};`);
  lines.push('static constexpr FileCommand fileCommands[]={'+fileCommands.map(row=>`{${q(row.name)},${row.guest},${row.host}}`).join(',')+'};');
  lines.push('static constexpr Variable variables[]={');variables.forEach(v=>lines.push(`{${q(v.name)},${v.type}},`));lines.push('};');
  lines.push('static constexpr Interface interfaces[]={'+interfaces.map(row=>`{${q(row.name)},${row.family},${row.type},${row.bytes}}`).join(',')+'};');
  lines.push('static constexpr InterfaceQuery interfaceQueries[]={'+interfaceQueries.map(row=>`{${row.type},${row.family},${row.identifier},${row.output}}`).join(',')+'};');
  lines.push('static constexpr TaggedRecord taggedRecords[]={'+taggedRecords.map(row=>`{${row.family},${row.guest},${row.host},${row.type}}`).join(',')+'};');
  lines.push(`static constexpr unsigned nativeAllocationAlignment=${allocationAlignment||1};`);
  const file=path.join(output,'native-imports.inc'),bytes=lines.join('\n')+'\n';
  if(!fs.existsSync(file)||fs.readFileSync(file,'utf8')!==bytes)fs.writeFileSync(file,bytes);
  return {types:types.length,functions:functions.length,variables:variables.length};
}
function walk(directory,prefix=''){
  return fs.readdirSync(directory,{withFileTypes:true}).flatMap(entry=>entry.isDirectory()?walk(path.join(directory,entry.name),prefix+entry.name+'/'):[prefix+entry.name]);
}

/** Published NDK C headers define the entire declaration universe. Internal
 * compiler, architecture and kernel headers are dependencies of those public
 * headers, never an independent competing ABI or a function allowlist. */
export async function extractTargetDeclarations({python,extractor,config,result,inputs,identity}){
  await cachedBuildStep({cache:result+'.reuse.json',inputs:[python,extractor,config,...inputs],files:[result],key:identity,
    build(){execFileSync(python,[extractor,config,result],{stdio:'inherit'});}});
  return JSON.parse(fs.readFileSync(result));
}

/** Re-emit both VM bindings from their one retained declaration supplier.
 * AST extraction and descriptor generation have separate dependencies: an
 * adapter role change must not require another parse of unchanged NDK headers. */
export function refreshDescriptors({output,include,python,graphicsRegistry,clang}){
  const file=path.join(output,'descriptor-inputs.json');
  const previous=fs.existsSync(file)?JSON.parse(fs.readFileSync(file)):{};
  include=include||previous.include||JSON.parse(fs.readFileSync(path.join(output,'guest-input.json'))).include;
  python=python||previous.python;
  if(graphicsRegistry===undefined){
    if(!Object.hasOwn(previous,'graphicsRegistry'))throw Error('Retained ARM32 declarations require their original graphics registry input');
    graphicsRegistry=previous.graphicsRegistry;
  }
  const guest=JSON.parse(fs.readFileSync(path.join(output,'guest-ast.json'))),host=JSON.parse(fs.readFileSync(path.join(output,'host-ast.json')));
  const notices=path.join(output,'notices');fs.mkdirSync(notices,{recursive:true});
  const graphicsLengths=graphicsRegistry?readGraphicsLengths({python,registry:graphicsRegistry,notice:path.join(notices,'khronos-gl-registry.txt')}):{};
  const mediaTags=readMediaTags({include,guest,host,clang});
  const result=emitDescriptors({guest,host,output,graphicsLengths,mediaTags});
  writeChanged(file,JSON.stringify({include:path.resolve(include),python:python?path.resolve(python):null,graphicsRegistry:graphicsRegistry?path.resolve(graphicsRegistry):null},null,2)+'\n');
  return result;
}

export async function generateImports({include,python,output,ownerHeaders=[],graphicsRegistry}){
  fs.mkdirSync(output,{recursive:true});include=path.resolve(include);
  const pin=JSON.parse(fs.readFileSync(path.join(root,'ndk-headers.json')));
  const sensitive=new Map(pin.caseSensitiveHeaders.map(row=>[row.path,row]));
  const folded=new Set([...sensitive.keys()].map(file=>file.toLowerCase()));
  const allFiles=[...walk(include).filter(file=>!folded.has(file.toLowerCase())),...sensitive.keys()].sort();
  const virtual=[];
  const identity=crypto.createHash('sha256');
  for(const file of allFiles){
    let actual=path.join(include,file),bytes=fs.readFileSync(actual),hash=crypto.createHash('sha256').update(bytes).digest('hex');
    const expected=sensitive.get(file);
    if(expected&&hash!==expected.sha256){actual=path.resolve(include,'../case-sensitive',expected.sha256);bytes=fs.readFileSync(actual);hash=crypto.createHash('sha256').update(bytes).digest('hex');}
    if(expected)virtual.push({type:'file',name:path.join(include,file),'external-contents':actual});
    identity.update(`${file}\0${bytes.length}\0${hash}\n`);
  }
  if(allFiles.length!==pin.files||identity.digest('hex')!==pin.treeSha256)throw Error('NDK header tree differs from ndk-headers.json');
  const overlay=path.resolve(output,'ndk-vfs.json');
  writeChanged(overlay,JSON.stringify({version:0,'case-sensitive':true,'use-external-names':false,roots:virtual}));
  const files=allFiles.filter(name=>name.endsWith('.h'));
  const dependencies=new Map([['bits','C library implementation headers'],['linux','Linux kernel ABI included by public system headers'],['asm-generic','Generic kernel definitions'],['xen','Hypervisor kernel definitions'],['c++','Guest C++ standard library owns its native C++ implementation']]);
  const vulkan=fs.readFileSync(path.join(include,'vulkan/vulkan.h'),'utf8');
  const otherPlatforms=new Set([...vulkan.matchAll(/#ifdef (VK_USE_PLATFORM_\w+)[\s\S]*?#include "([^"]+)"/g)].filter(match=>match[1]!=='VK_USE_PLATFORM_ANDROID_KHR').map(match=>'vulkan/'+match[2]));
  const inventory=files.map(file=>{
    const first=file.split('/')[0];
    const reason=dependencies.get(first)||(otherPlatforms.has(file)?'Original Vulkan umbrella selects this header only for another operating system':null)||(/-linux-(?:android|androideabi)$/.test(first)?'Target-specific definitions selected by Clang include order':null);
    return {file,role:reason?'dependency':'public',reason:reason||'Published C/NDK header'};
  });
  // Conditional command values remain compiler-owned just like structures.
  // Discover every name in the original fcntl headers, then let each target's
  // preprocessing/layout API supply active values and flock representations.
  const commands=new Set();for(const file of allFiles.filter(file=>/\/fcntl\.h$|^fcntl\.h$/.test(file)))for(const match of fs.readFileSync(path.join(include,file),'utf8').matchAll(/^\s*#\s*define\s+(F_\w+)\s+/gm))commands.add(match[1]);
  const fileCommands=path.resolve(output,'file-command-types.h');writeChanged(fileCommands,'#include <fcntl.h>\nvoid __maclauncher_file_types(struct flock*,struct flock64*);\n'+[...commands].sort().map(name=>`#ifdef ${name}\nextern char __maclauncher_file_command_${name}[${name}+1];\n#endif`).join('\n')+'\n');
  ownerHeaders=[...ownerHeaders,fileCommands];
  const headers=[...inventory.filter(row=>row.role==='public').map(row=>row.file),...ownerHeaders];
  // A newer API level intentionally hides legacy exported declarations (for
  // example the pre-23 standard streams). Consume both sides of every original
  // API conditional, instead of selecting legacy symbols by hand.
  const apiNames=Object.fromEntries([...fs.readFileSync(path.join(include,'android/api-level.h'),'utf8').matchAll(/^#define\s+(__ANDROID_API_\w+__)\s+(\d+)/gm)].map(m=>[m[1],Number(m[2])]));
  const headerLevels={};
  for(const header of headers){
    const source=fs.readFileSync(path.isAbsolute(header)?header:path.join(include,header),'utf8'),levels=new Set([36]);
    for(const match of source.matchAll(/^\s*#\s*(?:if|elif)\b[^\n]*__ANDROID_API__[^\n]*/gm)){
      const expression=match[0].replace(/__ANDROID_API_\w+__/g,name=>String(apiNames[name]??name));
      for(const value of expression.matchAll(/\b(\d+)\b/g))for(const level of [Number(value[1])-1,Number(value[1])])if(level>0&&level<=36)levels.add(level);
    }
    headerLevels[header]=[...levels].sort((a,b)=>b-a);
  }
  const headerPrelude={};
  for(const header of headers){
    // Khronos extension fragments intentionally rely on their base API header.
    const directory=header.split('/')[0],base={EGL:'EGL/egl.h',GLES:'GLES/gl.h',GLES2:'GLES2/gl2.h',GLES3:'GLES3/gl32.h',vulkan:'vulkan/vulkan_core.h'}[directory];
    headerPrelude[header]='#include <stddef.h>\n#include <stdint.h>\n'+(base?'#include <'+base+'>\n':'');
  }
  const targets={guest:'armv7a-linux-androideabi36',host:'aarch64-linux-android36'},data={};
  // The complete validated NDK tree above and Clang's own resource directory
  // identify the declaration supplier. Adapter edits which do not change that
  // supplier or the extractor need not parse every original header again.
  const clang=fs.realpathSync(execFileSync('xcrun',['--find','clang'],{encoding:'utf8'}).trim());
  generateLoaderAbi({include,output,clang});
  generateSignalAbi({include,output,clang});
  const libclang=fs.realpathSync(path.resolve(path.dirname(clang),'../lib/libclang.dylib'));
  const resources=path.join(execFileSync(clang,['-print-resource-dir'],{encoding:'utf8'}).trim(),'include');
  const resourceFiles=walk(resources).sort();
  const extractor=path.join(root,'imports-ast.py');
  const inputs=[overlay,...ownerHeaders,clang,libclang,...resourceFiles.map(file=>path.join(resources,file))];
  for(const [name,target] of Object.entries(targets)){
    const config=path.join(output,name+'-input.json'),result=path.join(output,name+'-ast.json');
    writeChanged(config,JSON.stringify({include,headers,target,headerPrelude,headerLevels,arguments:['-ivfsoverlay',overlay,'-DVK_USE_PLATFORM_ANDROID_KHR','-DGL_GLEXT_PROTOTYPES','-DEGL_EGLEXT_PROTOTYPES']}));
    data[name]=await extractTargetDeclarations({python,extractor,config,result,inputs,
      identity:{headerTree:pin.treeSha256,resources,resourceFiles}});
    const errors=data[name].headers.filter(row=>row.error||(row.errors?.length&&!row.originalTargetConstraint));
    if(errors.length)throw Error('Clang could not read original Android declarations: '+JSON.stringify(errors));
  }
  writeChanged(path.join(output,'header-inventory.json'),JSON.stringify(inventory,null,2)+'\n');
  const notices=path.join(output,'notices');fs.mkdirSync(notices,{recursive:true});
  refreshDescriptors({include,python,output,graphicsRegistry:graphicsRegistry||null,clang});
  let notice;
  for(let ancestor=path.dirname(include);;ancestor=path.dirname(ancestor)){
    const candidate=path.join(ancestor,'NOTICE');
    if(fs.existsSync(candidate)){
      const bytes=fs.readFileSync(candidate);
      if(crypto.createHash('sha256').update(bytes).digest('hex')===pin.noticeSha256){notice=bytes;break;}
    }
    if(path.dirname(ancestor)===ancestor)break;
  }
  if(!notice)throw Error('Original NDK NOTICE was not found above its header tree');
  writeChanged(path.join(notices,'android-ndk.txt'),notice);
  return {output,guest:data.guest,host:data.host};
}

if(process.argv[1]&&path.resolve(process.argv[1])===new URL(import.meta.url).pathname){
  if(process.argv[2]==='--refresh'){
    if(!process.argv[3])throw Error('Expected retained ARM32 declaration directory');
    console.log(JSON.stringify(refreshDescriptors({output:path.resolve(process.argv[3])})));
  }else{
  const [include,python,output]=process.argv.slice(2);if(!include||!python||!output)throw Error('Expected NDK include, Python and output paths');
  const result=await generateImports({include,python,output});
  console.log(JSON.stringify({guestFunctions:Object.keys(result.guest.functions).length,hostFunctions:Object.keys(result.host.functions).length,guestTypes:Object.keys(result.guest.types).length,hostTypes:Object.keys(result.host.types).length}));
  }
}
