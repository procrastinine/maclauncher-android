import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {exportSources} from '../binder-rpc/build.mjs';
import {preparePrctlConstants} from './prctl.mjs';
const pins=JSON.parse(fs.readFileSync(new URL('../source-pins.json',import.meta.url)));
const macros=text=>new Map([...text.matchAll(/^#define (E[A-Z0-9_]+)\s+([^\n]+)$/gm)].map(m=>[m[1],m[2].trim()]));
function values(definitions){
 const answer=new Map();
 function resolve(name,seen=new Set()){
  if(answer.has(name))return answer.get(name);if(seen.has(name))return null;seen.add(name);
  const value=definitions.get(name);if(value===undefined)return null;
  const number=/^\d+$/.test(value)?Number(value):/^[A-Z][A-Z0-9_]+$/.test(value)?resolve(value,seen):null;
  if(number!==null)answer.set(name,number);return number;
 }
 for(const name of definitions.keys())resolve(name);return answer;
}
/** Constant identities come from the complete upstream headers, never from
 * observed game errors. Native errno remains native until the guest boundary. */
export function prepareProcessAbi({bionicSource,build,libc}){
 const output=path.join(build,'process-abi'),source=path.join(output,'source');
 preparePrctlConstants({bionicSource,output});
 exportSources(bionicSource,pins.aospBionic,[
  'libc/kernel/uapi/asm-generic/errno-base.h',
  'libc/kernel/uapi/asm-generic/errno.h'
 ],source);
 const guest=values(macros(execFileSync('clang',['-target','aarch64-linux-android36','-dM','-E','-I'+path.join(source,'libc/kernel/uapi'),'-include','asm-generic/errno.h','-'],{input:'',encoding:'utf8'})));
 const native=values(macros(execFileSync('clang',['-dM','-E','-include','errno.h','-'],{input:'',encoding:'utf8'})));
 const pairs=[...guest].filter(([name])=>native.has(name)).map(([name,value])=>({name,native:native.get(name),guest:value}));
 // Different names for the same operation, plus host-only executable formats.
 const equivalent={ENOTSUP:'EOPNOTSUPP',ENOATTR:'ENODATA',EPROCLIM:'EAGAIN',EFTYPE:'EINVAL',EAUTH:'EACCES',ENEEDAUTH:'EACCES',EPWROFF:'ENODEV',EDEVERR:'EIO',EBADEXEC:'ENOEXEC',EBADARCH:'ENOEXEC',ESHLIBVERS:'ENOEXEC',EBADMACHO:'ENOEXEC',EQFULL:'ENOBUFS',ENOTCAPABLE:'EPERM'};
 for(const [host,name]of Object.entries(equivalent))if(native.has(host)&&!pairs.some(p=>p.native===native.get(host)))pairs.push({name:host,native:native.get(host),guest:guest.get(name)});
 let text=fs.readFileSync(libc,'utf8');
 const begin=text.indexOf('static const struct { int darwin, linux_; } g_errno_map[] = {'),end=text.indexOf('// bionic spells it __errno()',begin);
 if(begin<0||end<0)throw Error('Upstream errno owner changed');
 const replacement=`static const struct { int darwin, linux_; } g_errno_map[] = {\n${pairs.map(p=>`    {${p.native},${p.guest}}, // ${p.name}`).join('\n')}\n};
int kl_errno_to_linux(int value) { if(!value)return 0;for(size_t i=0;i<sizeof(g_errno_map)/sizeof(*g_errno_map);i++)if(g_errno_map[i].darwin==value)return g_errno_map[i].linux_;return 5; }
int kl_errno_from_linux(int value) { if(!value)return 0;for(size_t i=0;i<sizeof(g_errno_map)/sizeof(*g_errno_map);i++)if(g_errno_map[i].linux_==value)return g_errno_map[i].darwin;return -1; }
\n`;
 text=text.slice(0,begin)+replacement+text.slice(end);
 text=text.replace('errno = kl_errno_to_linux(EINVAL);','errno = EINVAL;').replace('            errno = kl_errno_to_linux(errno);\n','');
 fs.writeFileSync(libc,text);
 fs.writeFileSync(path.join(output,'constants.json'),JSON.stringify({revision:pins.aospBionic,pairs},null,2)+'\n');
 return {output,pairs};
}
