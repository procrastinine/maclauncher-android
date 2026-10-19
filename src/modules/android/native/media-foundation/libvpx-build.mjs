import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';
import {exportSources} from '../binder-rpc/build.mjs';
import {copyChanged,writeChanged} from '../upstream/source-files.mjs';
import {nativePathFlags,nativeFlagsString} from '../upstream/native-paths.mjs';

const root=import.meta.dirname;
const pin=JSON.parse(fs.readFileSync(path.join(root,'vpx-source.json'),'utf8'));
const digest=value=>crypto.createHash('sha256').update(value).digest('hex');
/** Original decoder-only libvpx is statically linked into the shared media
 * owner. Configure and compilation run in owned exports, never in a submodule. */
export function buildLibvpx({source=path.resolve(root,'../../../../..',pin.sourcePath),output}){
  output=path.resolve(output);fs.mkdirSync(output,{recursive:true});
  const original=path.join(output,'source'),build=path.join(output,'build');
  exportSources(source,pin.revision,['.'],original);
  fs.mkdirSync(build,{recursive:true});
  const compiler=execFileSync('xcrun',['--find','clang'],{encoding:'utf8'}).trim();
  const toolchain=execFileSync(compiler,['--version'],{encoding:'utf8'});
  const sdk=execFileSync('xcrun',['--sdk','macosx','--show-sdk-path'],{encoding:'utf8'}).trim();
  const flags=['-arch','arm64','-mmacosx-version-min='+pin.minimumMacOS,'-O2','-fPIC',...nativePathFlags({source:original,build:output})];
  const options=['--target=arm64-darwin22-gcc','--enable-pic','--enable-static','--disable-shared','--disable-examples','--disable-tools','--disable-docs','--disable-unit-tests','--disable-install-docs','--disable-vp8-encoder','--disable-vp9-encoder','--disable-vp9-highbitdepth','--disable-webm-io','--disable-libyuv'];
  const identity=digest(JSON.stringify({pin,options,flags,compiler,toolchain,sdk,helper:fs.readFileSync(import.meta.filename,'utf8')}));
  const stamp=path.join(build,'.maclauncher-configure.json');
  const prior=fs.existsSync(stamp)?JSON.parse(fs.readFileSync(stamp,'utf8')):null;
  if(prior?.identity!==identity){
    // Configure identities change only with consumed source/toolchain/options.
    // Retire obsolete objects instead of reusing incompatible Make rules.
    fs.rmSync(build,{recursive:true,force:true});fs.mkdirSync(build,{recursive:true});
    execFileSync(path.join(original,'configure'),options,{cwd:build,stdio:'inherit',env:{...process.env,CC:compiler,CXX:execFileSync('xcrun',['--find','clang++'],{encoding:'utf8'}).trim(),CFLAGS:nativeFlagsString(flags),CXXFLAGS:nativeFlagsString(flags),LDFLAGS:nativeFlagsString(['-arch','arm64','-mmacosx-version-min='+pin.minimumMacOS]),MACOSX_DEPLOYMENT_TARGET:pin.minimumMacOS,SDKROOT:sdk}});
    writeChanged(stamp,JSON.stringify({identity,version:pin.version,revision:pin.revision,options})+'\n');
  }
  execFileSync('make',['-j'+Math.max(1,Math.min(8,os.availableParallelism())),'libvpx.a'],{cwd:build,stdio:'inherit'});
  const library=path.join(output,'libvpx.a');copyChanged(path.join(build,'libvpx.a'),library);
  const notices=path.join(output,'notices');fs.mkdirSync(notices,{recursive:true});
  for(const file of ['LICENSE','PATENTS','AUTHORS'])copyChanged(path.join(original,file),path.join(notices,file));
  writeChanged(path.join(notices,'source.json'),JSON.stringify({...pin,license:'BSD-3-Clause; PATENTS grant',options},null,2)+'\n');
  return {library,include:original,source:original,notices};
}
