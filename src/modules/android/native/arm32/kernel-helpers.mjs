import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
const [compiler,source,output]=process.argv.slice(2),object=path.join(output,'kernel-helpers.o');
execFileSync(compiler,['--target=armv7a-linux-androideabi21','-c',source,'-o',object],{stdio:'inherit'});
const bytes=fs.readFileSync(object),sections=bytes.readUInt32LE(32),stride=bytes.readUInt16LE(46),count=bytes.readUInt16LE(48);
const strings=sections+bytes.readUInt16LE(50)*stride,names=bytes.readUInt32LE(strings+16);
let code;
for(let i=0;i<count;i++){
  const section=sections+i*stride,start=names+bytes.readUInt32LE(section);
  if(bytes.toString('utf8',start,bytes.indexOf(0,start))!=='.text')continue;
  const offset=bytes.readUInt32LE(section+16),size=bytes.readUInt32LE(section+20);code=bytes.subarray(offset,offset+size);
}
if(!code||code.length!==4096)throw Error('ARM kernel helper page must occupy exactly 4096 bytes');
fs.writeFileSync(path.join(output,'kernel-helpers.inc'),'static constexpr unsigned char kernelHelpers[] = {'+[...code].join(',')+'};\n');
