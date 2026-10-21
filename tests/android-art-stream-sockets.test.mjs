import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';

const adapter=new URL('../src/modules/android/art/adapters/aim-stream-sockets.patch',import.meta.url);
const fixture=new URL('./fixtures/android-art-sockets/stream-sockets.rs',import.meta.url);
const run=(command,args,options={})=>{
 const result=spawnSync(command,args,{encoding:'utf8',timeout:110000,maxBuffer:4<<20,...options});
 assert.ifError(result.error);assert.equal(result.signal,null,result.stdout+result.stderr);
 assert.equal(result.status,0,result.stdout+result.stderr);return result;
};
const declarations=text=>text.split('\n').filter(line=>/^\s*pub(?:\([^)]*\))?\s+(?:unsafe\s+)?(?:fn|struct|enum|type|trait|const|static|mod|use)\b/.test(line));

test('stream socket backports retain upstream identity and original public declarations',t=>{
 const patch=fs.readFileSync(adapter,'utf8');
 for(const commit of ['1a203861883b79edd91869a711fa96d857229df1','7f447f1ea559678bc6e280a3a1f7ce2d6921ccc7'])assert.ok(patch.includes(commit));
 assert.match(patch,/Apache-2\.0/);
 const producer=fs.readFileSync(new URL('../src/modules/android/art/source.mjs',import.meta.url),'utf8');
 assert.equal(producer.split("'aim-stream-sockets.patch'").length,2);
 const source=process.env.ANDROID_AIM_SOURCE;
 if(!source)return t.skip('Requires the existing pinned AIM source checkout');
 const temporary=fs.mkdtempSync(path.join(os.tmpdir(),'android-socket-source-'));
 t.after(()=>fs.rmSync(temporary,{recursive:true,force:true}));
 const relative='crates/aim-linux-abi/src/sys',directory=path.join(temporary,relative);
 fs.mkdirSync(directory,{recursive:true});
 const before={};
 for(const name of ['net.rs','fdtab.rs']){
  before[name]=fs.readFileSync(path.join(source,relative,name),'utf8');
  fs.writeFileSync(path.join(directory,name),before[name]);
 }
 run('git',['apply','--unsafe-paths',fileURLToPath(adapter)],{cwd:temporary});
 for(const name of ['net.rs','fdtab.rs'])assert.deepEqual(declarations(fs.readFileSync(path.join(directory,name),'utf8')),declarations(before[name]));
 const original=before['net.rs'],adapted=fs.readFileSync(path.join(directory,'net.rs'),'utf8');
 const codec=text=>text.slice(text.indexOf('pub(super) fn save_sock('),text.indexOf('enum SocketPin<'));
 assert.equal(codec(adapted),codec(original),'Existing fork socket state encoding remains unchanged');
});

test('complete Linux ABI socket boundary preserves real connect and SCM transfers',{timeout:120000},t=>{
 const prepared=process.env.ANDROID_ART_PREPARED_SOURCE,inputs=process.env.ANDROID_SOCKET_NATIVE_INPUTS;
 if(process.platform!=='darwin'||process.arch!=='arm64'||!prepared||!inputs)
  return t.skip('Requires a complete adapted AIM source projection and cached native compiler inputs');
 const native=JSON.parse(fs.readFileSync(inputs,'utf8'));
 assert.ok(native.rustc&&native.externs&&native.dependencyDirectories?.length);
 const temporary=fs.mkdtempSync(path.join(os.tmpdir(),'android-socket-boundary-'));
 t.after(()=>fs.rmSync(temporary,{recursive:true,force:true}));
 const project=path.join(temporary,'projection'),directory=path.join(project,'crates/aim-linux-abi/src');
 fs.mkdirSync(path.dirname(directory),{recursive:true});
 fs.cpSync(path.join(prepared,'crates/aim-linux-abi/src'),directory,{recursive:true});
 const patch=fileURLToPath(adapter);
 const check=spawnSync('git',['apply','--check','--unsafe-paths',patch],{cwd:project,encoding:'utf8'});
 if(check.status===0)run('git',['apply','--unsafe-paths',patch],{cwd:project});
 else run('git',['apply','--reverse','--check','--unsafe-paths',patch],{cwd:project});
 // The fixture is attached to the complete module in this private compilation.
 // All Linux syscalls, descriptor owners and Binder/SCM transports stay original.
 fs.appendFileSync(path.join(directory,'sys/net.rs'),'\n#[path='+JSON.stringify(fileURLToPath(fixture))+'] pub(crate) mod stream_socket_boundary_fixture;\n');
 fs.appendFileSync(path.join(directory,'sys/mod.rs'),'\npub(crate) use net::stream_socket_boundary_fixture::run as run_socket_boundary_fixture;\n');
 fs.appendFileSync(path.join(directory,'lib.rs'),'\nfn main(){sys::run_socket_boundary_fixture();}\n');
 const object=path.join(temporary,'x18-core.o'),archive=path.join(temporary,'libx18_core.a');
 run(process.env.CLANG||'clang',['-std=gnu11','-O2','-fPIC','-g0','-Werror','-c',path.join(directory,'x18-core/x18-core.c'),'-o',object]);
 run('ar',['crs',archive,object]);
 const executable=path.join(temporary,'stream-sockets');
 const args=['--edition=2024','--crate-name','stream_socket_boundary',path.join(directory,'lib.rs'),'-C','opt-level=1','-C','debuginfo=0','-o',executable];
 for(const dependency of native.dependencyDirectories)args.push('-L','dependency='+dependency);
 for(const [name,file]of Object.entries(native.externs)){
  assert.ok(fs.statSync(file).size>0,'Cached native dependency is complete: '+name);
  args.push('--extern',name+'='+file);
 }
 args.push('-L','native='+temporary,'-l','static=x18_core');
 run(native.rustc,args);
 const result=run(executable,[path.join(temporary,'runtime')],{timeout:15000});
 assert.match(result.stdout,/stream connect: EINPROGRESS -> 0 -> EISCONN; blocking retry; payload survives/);
 assert.match(result.stdout,/inherited socket: actual SCM_RIGHTS transfer survives sender close; allocated socket retains receipt/);
 t.diagnostic(result.stdout.trim());
});
