import fs from 'node:fs';
import path from 'node:path';
import {exportSources} from '../binder-rpc/build.mjs';
import {writeChanged,copyChanged} from '../upstream/source-files.mjs';
const pins=JSON.parse(fs.readFileSync(new URL('../source-pins.json',import.meta.url)));

// Keep the complete original constant namespace without importing Linux's
// native structure typedefs into a Darwin translation unit.
export function preparePrctlConstants({bionicSource,output}){
  const source=path.join(output,'source');
  exportSources(bionicSource,pins.aospBionic,['libc/kernel/uapi/linux/prctl.h','libc/kernel/uapi/linux/capability.h','libc/kernel/uapi/linux/securebits.h','libc/kernel/uapi/linux/netlink.h','libc/kernel/uapi/linux/fcntl.h','libc/kernel/uapi/linux/binfmts.h'],source);
  return writeProcessConstants({source,output});
}
export function writeProcessConstants({source,output}){
  fs.mkdirSync(output,{recursive:true});
  const original=fs.readFileSync(path.join(source,'libc/kernel/uapi/linux/prctl.h'),'utf8');
  const definitions=original.replace(/\\\n/g,' ').split('\n').filter(line=>/^#define PR_[A-Z0-9_]+\b/.test(line));
  if(!definitions.some(line=>line.startsWith('#define PR_SET_NAME '))||definitions.length<100)throw Error('Original process-control constants changed');
  const header=path.join(output,'prctl-constants.h');
  writeChanged(header,'// Constants from the complete pinned Bionic Linux UAPI header.\n#pragma once\n'+definitions.join('\n')+'\n');
  const capability=fs.readFileSync(path.join(source,'libc/kernel/uapi/linux/capability.h'),'utf8');
  const capabilities=capability.replace(/\\\n/g,' ').split('\n').filter(line=>/^#define (CAP_|_LINUX_CAPABILITY_|VFS_CAP_)[A-Z0-9_]+\b/.test(line));
  if(!capabilities.some(line=>line.startsWith('#define CAP_LAST_CAP ')))throw Error('Original capability UAPI changed');
  writeChanged(path.join(output,'capability-constants.h'),'// Complete pinned Bionic capability constants.\n#pragma once\n'+capabilities.join('\n')+'\n');
  copyChanged(path.join(source,'libc/kernel/uapi/linux/securebits.h'),path.join(output,'securebits.h'));
  const netlink=fs.readFileSync(path.join(source,'libc/kernel/uapi/linux/netlink.h'),'utf8').replace(/\\\n/g,' ').split('\n').filter(line=>/^#define NETLINK_[A-Z0-9_]+\b/.test(line));
  writeChanged(path.join(output,'netlink-constants.h'),'// Complete pinned Bionic netlink protocol constants.\n#pragma once\n'+netlink.join('\n')+'\n');
  const at=fs.readFileSync(path.join(source,'libc/kernel/uapi/linux/fcntl.h'),'utf8').replace(/\\\n/g,' ').split('\n')
    .filter(line=>/^#define AT_[A-Z0-9_]+\b/.test(line)).map(line=>line.replace(/\bAT_[A-Z0-9_]+\b/g,name=>'KLJ_LINUX_'+name));
  writeChanged(path.join(output,'at-constants.h'),'// Complete pinned Bionic at-operation constants; Darwin names stay distinct.\n#pragma once\n'+at.join('\n')+'\n');
  const binary=fs.readFileSync(path.join(source,'libc/kernel/uapi/linux/binfmts.h'),'utf8').split('\n').filter(line=>/^#define (BINPRM_|MAX_ARG_)[A-Z0-9_]+\b/.test(line));
  writeChanged(path.join(output,'binfmt-constants.h'),'// Pinned Linux executable-format limits.\n#pragma once\n'+binary.join('\n')+'\n');
  return header;
}
