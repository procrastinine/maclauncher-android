import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {writeChanged} from './source-files.mjs';
import {nativePathFlags,nativeFlagsString} from './native-paths.mjs';

const compilerEnvironment=['CC','CXX','CFLAGS','CXXFLAGS','CPPFLAGS','LDFLAGS','SDKROOT','DEVELOPER_DIR','MACOSX_DEPLOYMENT_TARGET',
  'CPATH','C_INCLUDE_PATH','CPLUS_INCLUDE_PATH','LIBRARY_PATH','PKG_CONFIG_PATH','PKG_CONFIG_LIBDIR'];
function tool(command,environment){
  const selected=fs.existsSync(command)?path.resolve(command):execFileSync('/usr/bin/which',[command],{encoding:'utf8',env:environment}).trim();
  const file=fs.realpathSync(selected),stat=fs.statSync(file,{bigint:true});
  return {file,identity:['dev','ino','size','mtimeNs','ctimeNs'].map(key=>String(stat[key])),
    version:execFileSync(file,['--version'],{encoding:'utf8',env:environment}).trim()};
}
function inputs(commands,environment){
  const compiler=name=>{const value=environment[name];if(!value)return name==='CC'?'clang':'clang++';return fs.existsSync(value)?value:value.split(/\s+/)[0];};
  const sdk=environment.SDKROOT||execFileSync('xcrun',['--show-sdk-path'],{encoding:'utf8',env:environment}).trim();
  const settings=path.join(sdk,'SDKSettings.json');
  return {tools:[...new Set([...commands,'clang','clang++',compiler('CC'),compiler('CXX')])].map(command=>tool(command,environment)),
    environment:Object.fromEntries([...new Set([...compilerEnvironment,...Object.keys(environment).filter(key=>/^(CMAKE_|MESON_)/.test(key))])].sort().map(key=>[key,environment[key]||null])),
    sdk:{path:sdk,settings:fs.existsSync(settings)?fs.readFileSync(settings,'utf8'):null}};
}
function previous(file){try{return JSON.parse(fs.readFileSync(file,'utf8'));}catch(error){if(error.code==='ENOENT'||error instanceof SyntaxError)return null;throw error;}}
function same(a,b){return JSON.stringify(a)===JSON.stringify(b);}

/** Build engines retain source/header dependency ownership. This record only
 * avoids repeating configuration when its actual tools/options are unchanged. */
export function configureCmake({source,build,args=[],ninja=process.env.NINJA||'ninja',environment=process.env,pathRoots=[]}){
  // The system Make on macOS compares timestamps at whole-second resolution.
  // The existing Ninja supplier preserves fast source/header edit detection.
  const generator=environment.CMAKE_GENERATOR||'Ninja';
  const portable=nativeFlagsString(nativePathFlags({source,build:path.dirname(build),roots:pathRoots}));
  const configuredArgs=['-G',generator,...(generator.startsWith('Ninja')?['-DCMAKE_MAKE_PROGRAM='+tool(ninja,environment).file]:[]),...args];
  for(const [language,variable] of [['C','CFLAGS'],['CXX','CXXFLAGS'],['ASM','ASMFLAGS']]){
    const prefix='-DCMAKE_'+language+'_FLAGS=',position=configuredArgs.findIndex(value=>value.startsWith(prefix));
    const value=prefix+(position<0?environment[variable]||'':configuredArgs[position].slice(prefix.length))+' '+portable;
    if(position<0)configuredArgs.push(value);else configuredArgs[position]=value;
  }
  const record=path.join(build,'maclauncher-configure.json'),state={args:configuredArgs,source:path.resolve(source),inputs:inputs(['cmake',...(generator.startsWith('Ninja')?[ninja]:[])],environment)},old=previous(record);
  const cache=path.join(build,'CMakeCache.txt');
  if(fs.existsSync(cache)&&same(old,state))return false;
  // Compiler and environment changes must not reuse cached compiler choices
  // or objects built under ambient include/SDK settings absent from depfiles.
  if(old&&!same(old.inputs,state.inputs)){
    if(fs.existsSync(cache))execFileSync('cmake',['--build',build,'--target','clean'],{stdio:'inherit',env:environment});
    fs.rmSync(cache,{force:true});
  }
  if(fs.existsSync(cache)&&!fs.readFileSync(cache,'utf8').includes('CMAKE_GENERATOR:INTERNAL='+generator+'\n'))fs.rmSync(cache,{force:true});
  execFileSync('cmake',['-S',source,'-B',build,...configuredArgs],{stdio:'inherit',env:environment});
  writeChanged(record,JSON.stringify(state)+'\n');return true;
}
export function configureMeson({source,build,args=[],meson='meson',ninja='ninja',pkgConfig='pkg-config',dependencies=null,environment=process.env}){
  const record=path.join(build,'maclauncher-configure.json'),state={args,source:path.resolve(source),dependencies,inputs:inputs([meson,ninja,pkgConfig],environment)},old=previous(record);
  const configured=fs.existsSync(path.join(build,'build.ninja'));
  if(configured&&same(old,state))return false;
  const changedCompiler=old&&!same(old.inputs,state.inputs);
  execFileSync(meson,['setup',...(configured?[changedCompiler?'--wipe':'--reconfigure',...(!changedCompiler&&(!old||!same(old.dependencies,dependencies))?['--clearcache']:[])]:[]),build,source,...args],{stdio:'inherit',env:environment});
  writeChanged(record,JSON.stringify(state)+'\n');return true;
}
