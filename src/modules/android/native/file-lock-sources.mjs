import {includeDependencies} from './upstream/source-includes.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
export function prepareFileLockSource(file){
 const root=path.dirname(fileURLToPath(import.meta.url));let text=fs.readFileSync(file,'utf8');
 const start=text.indexOf('    // struct flock is laid out differently'),end=text.indexOf('    default:',start);
 if(start<0||end<0||!text.slice(start,end).includes('case LX_F_GETLK: case LX_F_SETLK: case LX_F_SETLKW:'))throw Error('Klepton file-lock source changed');
 text=includeDependencies(text.slice(0,start)+
  '    case LX_F_GETLK: case LX_F_SETLK: case LX_F_SETLKW:\n    case 36: case 37: case 38:\n        return klj_file_lock(fd,cmd,arg);\n'+text.slice(end),'#include '+JSON.stringify(path.join(root,'file-lock.h')));
 fs.writeFileSync(file,text);
}
