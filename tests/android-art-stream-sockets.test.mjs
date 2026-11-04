import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';

const fixture=new URL('./fixtures/android-art-sockets/stream-sockets.rs',import.meta.url);
const run=(command,args,options={})=>{
 const result=spawnSync(command,args,{encoding:'utf8',timeout:110000,maxBuffer:4<<20,...options});
 assert.ifError(result.error);assert.equal(result.signal,null,result.stdout+result.stderr);
 assert.equal(result.status,0,result.stdout+result.stderr);return result;
};
test('stream socket fixes are supplied by the original upstream owner',t=>{
 const source=process.env.ANDROID_AIM_SOURCE;
 if(!source)return t.skip('Requires the existing pinned AIM source checkout');
 const sys=path.join(source,'crates/aim-linux-abi/src/sys');
 const net=fs.readFileSync(path.join(sys,'net.rs'),'utf8'),fdtab=fs.readFileSync(path.join(sys,'fdtab.rs'),'utf8');
 assert.match(net,/EALREADY if connecting && !nonblocking\(fd\) => connected\(fd\)/);
 assert.match(net,/EISCONN if connecting => 0/);
 assert.match(fdtab,/Some\(Kind::Sock\(socket\)\) if socket\.inode_allocation\.lock\(\)\.unwrap\(\)\.is_some\(\)/);
 const producer=fs.readFileSync(new URL('../src/modules/android/art/source.mjs',import.meta.url),'utf8');
 assert.ok(!producer.includes('aim-stream-sockets.patch'),'Upstream socket fixes are not applied twice');
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
 // The fixture is attached to the complete module in this private compilation.
 // All Linux syscalls, descriptor owners and Binder/SCM transports stay original.
 fs.appendFileSync(path.join(directory,'sys/net.rs'),'\n#[path='+JSON.stringify(fileURLToPath(fixture))+'] pub(crate) mod stream_socket_boundary_fixture;\n');
 fs.appendFileSync(path.join(directory,'sys/mod.rs'),'\npub(crate) use net::stream_socket_boundary_fixture::run as run_socket_boundary_fixture;\n');
 fs.appendFileSync(path.join(directory,'lib.rs'),'\nfn main(){sys::run_socket_boundary_fixture();}\n');
 const core=path.join(directory,'x18-core/x18-core.c');
 if(fs.existsSync(core)){
  const object=path.join(temporary,'x18-core.o'),archive=path.join(temporary,'libx18_core.a');
  run(process.env.CLANG||'clang',['-std=gnu11','-O2','-fPIC','-g0','-Werror','-c',core,'-o',object]);
  run('ar',['crs',archive,object]);
 }
 const executable=path.join(temporary,'stream-sockets');
 const args=['--edition=2024','--crate-name','stream_socket_boundary',path.join(directory,'lib.rs'),'-C','opt-level=1','-C','debuginfo=0','-o',executable];
 for(const dependency of native.dependencyDirectories)args.push('-L','dependency='+dependency);
 for(const [name,file]of Object.entries(native.externs)){
  assert.ok(fs.statSync(file).size>0,'Cached native dependency is complete: '+name);
  args.push('--extern',name+'='+file);
 }
 if(fs.existsSync(core))args.push('-L','native='+temporary,'-l','static=x18_core');
 run(native.rustc,args);
 const result=run(executable,[path.join(temporary,'runtime')],{timeout:15000,cwd:temporary,env:{...process.env,TMPDIR:'.'}});
 assert.match(result.stdout,/stream connect: EINPROGRESS -> 0 -> EISCONN; blocking retry; payload survives/);
 assert.match(result.stdout,/inherited socket: actual SCM_RIGHTS transfer survives sender close; allocated socket retains receipt/);
 t.diagnostic(result.stdout.trim());
});
