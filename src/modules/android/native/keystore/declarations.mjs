import fs from 'node:fs';
import path from 'node:path';

// Keep source positions while excluding comments and literals from structural
// matching. Rust lifetimes remain code; quoted character literals do not.
function code(source) {
  let result='',index=0;
  const hide=end=>{result+=source.slice(index,end).replace(/[^\n]/g,' ');index=end;};
  while(index<source.length){
    if(source.startsWith('//',index)){const end=source.indexOf('\n',index);hide(end<0?source.length:end);continue;}
    if(source.startsWith('/*',index)){
      let end=index+2,depth=1;
      while(depth&&end<source.length){if(source.startsWith('/*',end)){depth++;end+=2;}else if(source.startsWith('*/',end)){depth--;end+=2;}else end++;}
      if(depth)throw Error('Unterminated service declaration comment');hide(end);continue;
    }
    const raw=source.slice(index).match(/^(?:br|r)(#*)"/);
    if(raw){const suffix='"'+raw[1],end=source.indexOf(suffix,index+raw[0].length);if(end<0)throw Error('Unterminated service declaration literal');hide(end+suffix.length);continue;}
    if(source[index]==='"'){
      let end=index+1;while(end<source.length&&source[end]!=='"'){if(source[end]==='\\')end++;end++;}
      if(end===source.length)throw Error('Unterminated service declaration literal');hide(end+1);continue;
    }
    const character=source.slice(index).match(/^'(?:\\(?:u\{[\da-fA-F]+\}|.)|[^'\\\r\n])'/);
    if(character){hide(index+character[0].length);continue;}
    result+=source[index++];
  }
  return result;
}

function endOf(text,start,open='(',close=')') {
  let depth=0;
  for(let index=start;index<text.length;index++){
    if(text[index]===open)depth++;
    else if(text[index]===close&&!--depth)return index;
  }
  throw Error('Unterminated service publisher declaration');
}

function argumentsAt(source,masked,start) {
  const end=endOf(masked,start),arguments_=[];let first=start+1,depth=0;
  for(let index=first;index<end;index++){
    if('([{<'.includes(masked[index]))depth++;
    else if(')]}>'.includes(masked[index]))depth--;
    else if(masked[index]===','&&!depth){arguments_.push(source.slice(first,index).trim());first=index+1;}
  }
  arguments_.push(source.slice(first,end).trim());return arguments_;
}

function descriptors(keystore) {
  const result=new Map();
  for(const directory of ['rust/generated-aidl','keymint-aidl-generated']){
    const root=path.join(keystore,directory);
    for(const relative of fs.globSync('**/*.rs',{cwd:root}).sort()){
      const source=fs.readFileSync(path.join(root,relative),'utf8');
      for(const match of source.matchAll(/\b(\w+)\s*\["([\w.]+)"\]\s*\{/g)){
        const previous=result.get(match[1]);
        if(previous&&previous!==match[2])throw Error('Ambiguous original service interface: '+match[1]);
        result.set(match[1],match[2]);
      }
    }
  }
  if(!result.size)throw Error('Original KeyStore AIDL declarations are unavailable');
  return result;
}

function rustDeclarations(keystore,interfaces,add) {
  const root=path.join(keystore,'source/security/keystore2');
  // The Rust builder copies the upstream-declared entry into this crate before
  // compiling it. Read that exact publisher, including every registration.
  const source=fs.readFileSync(path.join(keystore,'rust/platform/keystore_bootstrap/lib.rs'),'utf8'),masked=code(source),constants=new Map(),bindings=new Map();
  for(const match of source.matchAll(/\b(?:static|const)\s+(\w+)\s*:\s*&(?:'static\s+)?str\s*=\s*("(?:\\.|[^"\\])*")\s*;/g))
    if(masked.slice(match.index,match.index+6).trim())constants.set(match[1],JSON.parse(match[2]));
  for(const match of masked.matchAll(/\blet\s+(\([^=;]+\)|\w+)\s*=\s*([\w:]+)::(\w+)\s*\(/g))
    for(const variable of match[1].replace(/[()]/g,'').split(',').map(value=>value.trim()).filter(Boolean))bindings.set(variable,{type:match[2].split('::').at(-1),factory:match[3]});
  const sources=fs.globSync('**/*.rs',{cwd:root}).sort().map(file=>code(fs.readFileSync(path.join(root,file),'utf8')));
  function factoryInterface({type,factory}){
    const returns=new Set();
    for(const text of sources)for(const match of text.matchAll(new RegExp('\\bimpl\\s+'+type+'\\s*\\{','g'))){
      const start=text.indexOf('{',match.index),body=text.slice(start+1,endOf(text,start,'{','}'));
      for(const method of body.matchAll(new RegExp('\\bfn\\s+'+factory+'\\s*\\(','g'))){
        const args=body.indexOf('(',method.index),tail=body.slice(endOf(body,args)+1),signature=tail.slice(0,tail.indexOf('{'));
        for(const result of signature.matchAll(/\bStrong\s*<\s*dyn\s+([\w:]+)\s*>/g))returns.add(result[1].split('::').at(-1));
      }
    }
    if(returns.size!==1)throw Error('Original service factory has no unique Binder interface: '+type+'::'+factory);
    const name=[...returns][0],descriptor=interfaces.get(name);
    if(!descriptor)throw Error('Original service AIDL descriptor is unavailable: '+name);
    return descriptor;
  }
  let count=0;
  for(const match of masked.matchAll(/\badd_service\s*\(/g)){
    const args=argumentsAt(source,masked,masked.indexOf('(',match.index)),name=constants.get(args[0]),binding=args[1]?.match(/^(\w+)\.as_binder\(\)$/)?.[1],factory=bindings.get(binding);
    if(args.length!==2||!name||!factory)throw Error('Original KeyStore service registration changed: '+args[0]);
    add(name,factoryInterface(factory));count++;
  }
  if(!count)throw Error('Original KeyStore service registrations are unavailable');
}

function keyMintDeclarations(root,keystore,interfaces,add) {
  const source=fs.readFileSync(path.join(root,'keystore/KeyStoreBootstrap.cpp'),'utf8'),masked=code(source),bindings=new Map();
  for(const match of masked.matchAll(/\bauto\s+(\w+)\s*=\s*ndk::SharedRefBase::make<([\w:]+)>\s*\(/g))bindings.set(match[1],match[2]);
  const initialize=masked.match(/\bvoid\s+initialize\s*\(/);
  if(!initialize)throw Error('KeyMint service publisher is unavailable');
  const start=masked.indexOf('{',initialize.index),end=endOf(masked,start,'{','}');let count=0;
  for(const match of masked.slice(start,end).matchAll(/\bpublish\s*\(/g)){
    const offset=start+match.index,args=argumentsAt(source,masked,masked.indexOf('(',offset));
    const type=(args[0]?.match(/^ndk::SharedRefBase::make<([\w:]+)>/)?.[1]||bindings.get(args[0]))?.split('::').at(-1);
    if(args.length!==2||!type||!/^"(?:\\.|[^"\\])*"$/.test(args[1]))throw Error('KeyMint service publication changed');
    const header=code(fs.readFileSync(path.join(keystore,'source/keymaster/ng/include',type+'.h'),'utf8'));
    const base=header.match(new RegExp('\\bclass\\s+'+type+'\\s*:\\s*public\\s+(?:[\\w]+::)*(Bn\\w+)\\s*\\{'))?.[1];
    const descriptor=base&&interfaces.get('I'+base.slice(2));
    if(!descriptor)throw Error('Original KeyMint publisher interface is unavailable: '+type);
    add(descriptor+JSON.parse(args[1]),descriptor);count++;
  }
  if(!count)throw Error('KeyMint service registrations are unavailable');
  // Every native publication goes through the typed descriptor publisher above.
  if([...masked.matchAll(/\bAServiceManager_addService\s*\(/g)].length!==1)throw Error('KeyMint service publication boundary changed');
}

/** Derive every published service from the selected original source and AIDL output. */
export function keyStoreDeclarations({root,build}) {
  const keystore=path.join(build,'keystore'),interfaces=descriptors(keystore),result=new Map();
  const add=(name,descriptor)=>{
    if(!/^[\w./-]+$/.test(name)||!/^\w+(?:\.\w+)+$/.test(descriptor))throw Error('Invalid original KeyStore service declaration');
    if(result.has(name))throw Error('Duplicate original KeyStore service declaration: '+name);
    result.set(name,descriptor);
  };
  rustDeclarations(keystore,interfaces,add);keyMintDeclarations(root,keystore,interfaces,add);
  return new Map([...result].sort(([left],[right])=>left.localeCompare(right)));
}

export function writeKeyStoreDeclarations({root,build,output}) {
  const declarations=keyStoreDeclarations({root,build});
  fs.mkdirSync(path.dirname(output),{recursive:true});
  fs.writeFileSync(output,[...declarations].map(([name,descriptor])=>name+'='+descriptor+'\n').join(''));
  return declarations;
}
