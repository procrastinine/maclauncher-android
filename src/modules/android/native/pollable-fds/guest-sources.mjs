import {includeDependencies} from '../upstream/source-includes.mjs';
import fs from 'node:fs';
import path from 'node:path';
export function preparePollableSources({slink}) {
  let source=fs.readFileSync(slink,'utf8');
  const families=[
    ['// ------------------------------------------------------------------- epoll','// ------------------------------------------------------- *at() file syscalls',[
      ['int','epoll_create','int size','int','size'],
      ['int','epoll_create1','int flags','int','flags'],
      ['int','epoll_ctl','int fd,int op,int target,void *event','int,int,int,void *','fd,op,target,event'],
      ['int','epoll_wait','int fd,void *events,int count,int timeout','int,void *,int,int','fd,events,count,timeout']]],
    ['// ---- eventfd ----','// ---- ppoll ----',[
      ['int','eventfd','unsigned value,int flags','unsigned,int','value,flags'],
      ['int','eventfd_read','int fd,uint64_t *value','int,uint64_t *','fd,value'],
      ['int','eventfd_write','int fd,uint64_t value','int,uint64_t','fd,value']]]
  ];
  for(const [first,last,records]of families){
    const begin=source.indexOf(first),end=source.indexOf(last,begin);
    if(begin<0||end<0)throw Error('Pinned pollable descriptor boundary changed');
    const body=records.map(([result,name,args,types,values])=>`${result} klb_${name}(${args}){${result}(*call)(${types})=klj_pollable_lookup("${name}");return call(${values});}`).join('\n');
    source=source.slice(0,begin)+body+'\n\n'+source.slice(end);
  }
  fs.writeFileSync(slink,includeDependencies(source,'#include '+JSON.stringify(path.join(import.meta.dirname,'bridge.h'))));
}
