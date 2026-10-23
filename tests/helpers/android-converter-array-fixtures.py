from pathlib import Path
import struct,hashlib,zlib,sys
work=Path(sys.argv[1]).resolve()
p16=lambda x:struct.pack('<H',x&65535)
p32=lambda x:struct.pack('<I',x&0xffffffff)
def uleb(x):
 out=bytearray()
 while x>=128:out.append((x&127)|128);x>>=7
 out.append(x);return out
patterns={'Z':(1,[0,1]),'B':(1,[0,1,127,128,255,42,201]),'S':(2,[0,1,32767,32768,65535,12345]),'C':(2,[0,1,65535,55296,32768,12345]),'I':(4,[0,1,0x7fffffff,0x80000000,0xffffffff,0xdeadbeef]),'F':(4,[0,0x80000000,0x3f800000,0x7f800000,0xff800000,0x7fc00001,0x7fa12345]),'J':(8,[0,1,0x7fffffffffffffff,0x8000000000000000,0xffffffffffffffff,0xdeadbeef12345678]),'D':(8,[0,0x8000000000000000,0x3ff0000000000000,0x7ff0000000000000,0xfff0000000000000,0x7ff8000000000001,0x7ff123456789abcd])}
def build(kind,count,target,interface=False):
 width,values=patterns[kind];payload=b''.join(values[i%len(values)].to_bytes(width,'little')for i in range(count))
 owner='Lfixture/ArrayPayload'+kind+';';types0=[owner,'Ljava/lang/Object;','['+kind,'I','V'];shorties=['IL','VL','VLL'];names=['fill','catchFill','monitorFill']
 strings=sorted(set(types0+shorties+names));si={s:i for i,s in enumerate(strings)};types=sorted(types0,key=si.get);ti={s:i for i,s in enumerate(types)}
 protos=sorted([('I',('['+kind,),'IL'),('V',('['+kind,),'VL'),('V',('Ljava/lang/Object;','['+kind),'VLL')],key=lambda x:(ti[x[0]],tuple(ti[p]for p in x[1])))
 methods=sorted([(name,next(i for i,p in enumerate(protos)if p[2]==shorty))for name,shorty in [('fill','VL'),('catchFill','IL'),('monitorFill','VLL')]],key=lambda x:(si[x[0]],x[1]))
 offsets={};off=112
 for name,number,size in [('strings',len(strings),4),('types',len(types),4),('protos',len(protos),12),('methods',len(methods),8),('classes',1,32)]:offsets[name]=off;off+=number*size
 dataoff=off;data=bytearray(off)
 def append(blob,alignment=1):
  while len(data)%alignment:data.append(0)
  start=len(data);data.extend(blob);return start
 stringoffsets=[append(uleb(len(s))+s.encode()+b'\0')for s in strings]
 paramoffsets=[append(p32(len(p[1]))+b''.join(p16(ti[x])for x in p[1]),4)for p in protos]
 codeoffsets=[]
 for name,proto in methods:
  if name=='fill':regs,insn,tries=1,[0x0026,4,0,0x000e],b'';start,handler=0,0
  elif name=='catchFill':regs,insn,tries=2,[0x0126,8,0,0x1012,0x000f,0x000d,0xf012,0x000f],b'yes';start,handler=0,5
  else:regs,insn,tries=3,[0x011d,0x0226,9,0,0x011e,0x000e,0x000d,0x011e,0x0027,0],b'yes';start,handler=1,6
  instructions=b''.join(p16(x)for x in insn)+p16(0x300)+p16(width)+p32(count)+payload
  if len(instructions)%2:instructions+=b'\0'
  units=len(instructions)//2;body=p16(regs)+p16(len(protos[proto][1]))+p16(0)+p16(bool(tries))+p32(0)+p32(units)+instructions
  if tries:
   if units%2:body+=p16(0)
   body+=p32(start)+p16(3)+p16(1)+uleb(1)+b'\0'+uleb(handler)
  codeoffsets.append(append(body,4))
 classdata=uleb(0)+uleb(0)+uleb(3)+uleb(0)
 for index,code in enumerate(codeoffsets):classdata+=uleb(0 if index==0 else 1)+uleb(9)+uleb(code)
 classoff=append(classdata)
 maps=[(0x0000,1,0),(0x0001,len(strings),offsets['strings']),(0x0002,len(types),offsets['types']),(0x0003,len(protos),offsets['protos']),(0x0005,len(methods),offsets['methods']),(0x0006,1,offsets['classes']),(0x2002,len(strings),stringoffsets[0]),(0x1001,len(protos),paramoffsets[0]),(0x2001,3,codeoffsets[0]),(0x2000,1,classoff)]
 while len(data)%4:data.append(0)
 mapoff=len(data);maps.append((0x1000,1,mapoff));maps.sort(key=lambda x:x[2]);append(p32(len(maps))+b''.join(p16(t)+p16(0)+p32(n)+p32(o)for t,n,o in maps))
 for i,x in enumerate(stringoffsets):data[offsets['strings']+i*4:offsets['strings']+(i+1)*4]=p32(x)
 for i,t in enumerate(types):data[offsets['types']+i*4:offsets['types']+(i+1)*4]=p32(si[t])
 for i,p in enumerate(protos):data[offsets['protos']+i*12:offsets['protos']+(i+1)*12]=p32(si[p[2]])+p32(ti[p[0]])+p32(paramoffsets[i])
 for i,(name,proto)in enumerate(methods):data[offsets['methods']+i*8:offsets['methods']+(i+1)*8]=p16(ti[owner])+p16(proto)+p32(si[name])
 data[offsets['classes']:offsets['classes']+32]=b''.join(p32(x)for x in [ti[owner],0x601 if interface else 1,ti['Ljava/lang/Object;'],0,0xffffffff,0,classoff,0])
 header=[len(data),112,0x12345678,0,0,mapoff,len(strings),offsets['strings'],len(types),offsets['types'],len(protos),offsets['protos'],0,0,len(methods),offsets['methods'],1,offsets['classes'],len(data)-dataoff,dataoff]
 data[:112]=b'dex\n035\0'+b'\0'*24+b''.join(p32(x)for x in header);data[12:32]=hashlib.sha1(data[32:]).digest();data[8:12]=p32(zlib.adler32(data[12:]))
 target.mkdir(parents=True,exist_ok=True);(target/('ArrayPayload'+kind+'.dex')).write_bytes(data);(target/('ArrayPayload'+kind+'.raw')).write_bytes(payload)
for count,name in [(12000,'large'),(32,'small')]:
 for kind in patterns:build(kind,count,work/'fixtures'/name)

build("B",12000,work/"fixtures/interface",True)
