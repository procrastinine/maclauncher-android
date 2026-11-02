import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import {spawn,execFile} from 'node:child_process';
import {once} from 'node:events';
import {promisify} from 'node:util';
import {setTimeout as delay} from 'node:timers/promises';
const run=promisify(execFile);
const exists=file=>file&&fs.existsSync(file)&&fs.statSync(file).isFile();
const stdinHelper=process.env.ANDROID_CLIPBOARD_HELPER||(process.env.KLEPTON_NATIVE_RUNTIME&&path.join(process.env.KLEPTON_NATIVE_RUNTIME,'native/android-clipboard'));
const socketHelper=process.env.ANDROID_CLIPBOARD_SOCKET_HELPER;
function ready(child,socket){
 return new Promise((resolve,reject)=>{
  let pending='';
  const timer=setTimeout(()=>{cleanup();reject(Error('Existing clipboard helper did not answer its inert lifetime probe.'));},5000);
  const error=err=>{cleanup();reject(err);},closed=()=>error(Error('Clipboard helper exited before its lifetime probe.'));
  const cleanup=()=>{clearTimeout(timer);child.off('error',error);child.off('close',closed);child.stdout.off('data',data);};
  const data=bytes=>{pending+=bytes;let index;while((index=pending.indexOf('\n'))>=0){const line=pending.slice(0,index);pending=pending.slice(index+1);try{if(socket?line==='ready':JSON.parse(line).id==='lifetime'){cleanup();resolve();return;}}catch{}}};
  child.on('error',error);child.on('close',closed);child.stdout.on('data',data);
  if(!socket)child.stdin.write(JSON.stringify({op:'probe',id:'lifetime'})+'\n');
 });
}
async function cpuTime(pid){
 const {stdout}=await run('/bin/ps',['-p',String(pid),'-o','time=']);
 return stdout.trim().split(':').reduce((seconds,part)=>seconds*60+Number(part),0);
}
async function state(pid){
 try{return(await run('/bin/ps',['-p',String(pid),'-o','stat='])).stdout.trim();}catch(error){if(error.code===1)return '';throw error;}
}
async function gone(pid){
 const deadline=Date.now()+2500;
 while(Date.now()<deadline){const value=await state(pid);if(!value||value.startsWith('Z'))return;await delay(50);}
 assert.fail('The existing clipboard helper survived its parent with an unclosed native run loop.');
}
for(const[mode,binary]of [['stdin',stdinHelper],['socket',socketHelper]])test('existing '+mode+' clipboard helper exits on EOF and parent death without an idle spin',{timeout:15000},async t=>{
 if(process.platform!=='darwin'||!exists(binary))return t.skip('Requires an existing '+mode+' clipboard helper; this test never builds or downloads one.');
 const root=fs.mkdtempSync(path.join(os.tmpdir(),'clip-life-')),socket=mode==='socket'?path.join(root,'s.sock'):null;
 t.after(()=>fs.rmSync(root,{recursive:true,force:true}));
 const child=spawn(binary,socket?[socket]:[],{stdio:['pipe','pipe','ignore']});child.stdin.on('error',()=>{});
 t.after(()=>{if(child.exitCode==null&&child.signalCode==null)child.kill('SIGKILL');});
 await ready(child,socket);const before=await cpuTime(child.pid);await delay(350);const idle=await cpuTime(child.pid)-before;
 assert.ok(idle<0.15,'Unenrolled helper used '+idle+' CPU seconds during a 350 ms idle interval.');
 const closed=once(child,'close');child.stdin.end();const [code,signal]=await Promise.race([closed,delay(2500).then(()=>{throw Error('Native helper did not exit at stdin EOF.');})]);
 assert.equal(code,0);assert.equal(signal,null);if(socket)assert.equal(fs.existsSync(socket),false);
 // This parent owns the only stdin writer. Killing it tests native EOF
 // retirement without the app wrapper's two-second forced-kill fallback.
 const parentFile=path.join(root,'parent.cjs');
 fs.writeFileSync(parentFile,`const {spawn}=require('node:child_process');const socket=process.argv[3]||null;const child=spawn(process.argv[2],socket?[socket]:[],{stdio:['pipe','pipe','ignore']});child.stdin.on('error',()=>{});let pending='';child.stdout.on('data',bytes=>{pending+=bytes;let index;while((index=pending.indexOf('\\n'))>=0){const line=pending.slice(0,index);pending=pending.slice(index+1);try{if(socket?line==='ready':JSON.parse(line).id==='lifetime')process.send({pid:child.pid});}catch{}}});child.on('error',error=>process.send({error:error.message}));if(!socket)child.stdin.write(JSON.stringify({op:'probe',id:'lifetime'})+'\\n');`);
 const parent=spawn(process.execPath,[parentFile,binary,...(socket?[socket]:[])],{stdio:['ignore','ignore','ignore','ipc']});let orphan;
 t.after(async()=>{if(parent.exitCode==null&&parent.signalCode==null)parent.kill('SIGKILL');if(orphan){const value=await state(orphan);if(value&&!value.startsWith('Z'))try{process.kill(orphan,'SIGKILL');}catch{}}});
 const [message]=await Promise.race([once(parent,'message'),delay(5000).then(()=>{throw Error('The helper parent did not become ready.');})]);assert.equal(message.error,undefined);orphan=message.pid;assert.ok(Number.isSafeInteger(orphan));
 const parentExit=once(parent,'close');parent.kill('SIGKILL');await parentExit;await gone(orphan);if(socket)assert.equal(fs.existsSync(socket),false);
 t.diagnostic(JSON.stringify({binary,sha256:crypto.createHash('sha256').update(fs.readFileSync(binary)).digest('hex'),idleCpuSeconds:idle,eofExit:code,parentDeathExit:true}));
});
