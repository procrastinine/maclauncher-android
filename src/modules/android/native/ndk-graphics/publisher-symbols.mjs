import fs from 'node:fs';

/** Read function addresses from the verified publisher image. These are build
 * inputs, not a second runtime symbol registry or a list of permitted APIs. */
export function publisherFunctions(file) {
  const bytes=fs.readFileSync(file);
  const requireBytes=(offset,length)=>{if(offset<0||offset+length>bytes.length)throw Error('Truncated publisher Mach-O');};
  requireBytes(0,32);
  if(bytes.readUInt32LE(0)!==0xfeedfacf||bytes.readUInt32LE(4)!==0x100000c)throw Error('Expected arm64 publisher Mach-O');
  const sections=[null];let symbols,imageBase=0,cursor=32;
  for(let i=0;i<bytes.readUInt32LE(16);i++){
    requireBytes(cursor,8);const command=bytes.readUInt32LE(cursor),size=bytes.readUInt32LE(cursor+4);requireBytes(cursor,size);
    if(size<8)throw Error('Invalid Mach-O load command');
    if(command===2)symbols={offset:bytes.readUInt32LE(cursor+8),count:bytes.readUInt32LE(cursor+12),strings:bytes.readUInt32LE(cursor+16),size:bytes.readUInt32LE(cursor+20)};
    if(command===0x19){
      const name=bytes.toString('utf8',cursor+8,cursor+24).split('\0')[0];
      if(name==='__TEXT')imageBase=Number(bytes.readBigUInt64LE(cursor+24));
      for(let j=0;j<bytes.readUInt32LE(cursor+64);j++){
        const start=cursor+72+j*80;requireBytes(start,80);
        sections.push({flags:bytes.readUInt32LE(start+64)});
      }
    }
    cursor+=size;
  }
  if(!symbols)throw Error('Publisher symbol table is unavailable');
  requireBytes(symbols.offset,symbols.count*16);requireBytes(symbols.strings,symbols.size);
  const result=new Map();
  for(let i=0;i<symbols.count;i++){
    const offset=symbols.offset+i*16,type=bytes[offset+4],section=sections[bytes[offset+5]];
    if(type&0xe0||(type&0x0e)!==0x0e||!section||(section.flags&0x80000400)===0)continue;
    const start=bytes.readUInt32LE(offset);if(start>=symbols.size)throw Error('Invalid publisher symbol');
    const end=bytes.indexOf(0,symbols.strings+start);if(end<0||end>=symbols.strings+symbols.size)throw Error('Invalid publisher symbol name');
    const name=bytes.toString('utf8',symbols.strings+start,end),address=Number(bytes.readBigUInt64LE(offset+8))-imageBase;
    if(result.has(name)&&result.get(name)!==address)result.set(name,null);
    else result.set(name,address);
  }
  return result;
}
