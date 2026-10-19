import fs from 'node:fs';
import path from 'node:path';
import {writeChanged} from './upstream/source-files.mjs';
const header=path.join(import.meta.dirname,'android-log.h');
const replace=(source,from,to)=>{if(source.split(from).length!==2)throw Error('Android logging boundary changed: '+from.slice(0,70));return source.replace(from,to);};

// The original NDK owns the logger callback record/version contract, just as
// the existing allocator adapter derives its record from the published header.
export function prepareAndroidLogAbi({include,output}){
 const source=fs.readFileSync(path.join(include,'android/log.h'),'utf8');
 const record=source.match(/struct __android_log_message \{[\s\S]*?\n\};/)?.[0];
 const callback=source.match(/typedef void \(\*__android_logger_function\)\([^;]+;/)?.[0];
 if(!record||!callback)throw Error('Original Android logger callback declaration is unavailable');
 const file=path.join(output,'android-log-abi.h');fs.mkdirSync(output,{recursive:true});
 writeChanged(file,source.match(/^\/\*[\s\S]*?\*\//)[0]+'\n#pragma once\n#include <stddef.h>\n#include <stdint.h>\n'+record+'\n'+callback+'\n');
 return file;
}

export function prepareAndroidLogSources({shim,slink,handlers}){
 let source=fs.readFileSync(shim,'utf8');
 source=replace(source,'    return fprintf(stderr, "[%d/%s] %s\\n", p, t ? t : "", m ? m : ""); }',
   '    return klj_android_log_write(p,t,m); }');
 writeChanged(shim,'#include '+JSON.stringify(header)+'\n'+source);
 source=fs.readFileSync(slink,'utf8');
 const start=source.indexOf('// ---- dup2/dup3, and the guest\'s attempt to own our stdout ----'),end=source.indexOf('int klb_dup2(',start);
 if(start<0||end<0)throw Error('Klepton stdio redirection boundary changed');
 source=source.slice(0,start)+'// Android logging owns its physical output independently of guest stdio.\n'+source.slice(end);
 source=source.replaceAll('    if (kl_fd_hijack_refused(to)) return to;\n','');
 writeChanged(slink,source);
 source=fs.readFileSync(handlers,'utf8');
 source=replace(source,`    static const char lv[] = "??VDIWEF";
    fprintf(stderr, "%c/%s: ", (prio >= 0 && prio < 8) ? lv[prio] : '?', tag ? tag : "");
    int n = vfprintf(stderr, fmt, _ap);
    fputc('\\n', stderr);`,
 `    char *message=NULL;int n=vasprintf(&message,fmt,_ap);
    if(n>=0){klj_android_log_write(prio,tag,message);free(message);}`);
 writeChanged(handlers,'#include '+JSON.stringify(header)+'\n'+source);
}
