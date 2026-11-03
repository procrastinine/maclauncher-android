import {copyOptional} from '../upstream/source-files.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {exportSources} from '../binder-rpc/build.mjs';
const pins=JSON.parse(fs.readFileSync(new URL('../source-pins.json',import.meta.url)));
const catalog=JSON.parse(fs.readFileSync(new URL('../keystore/sources.json',import.meta.url)));
const root=path.resolve(import.meta.dirname,'..');
const replace=(text,from,to)=>{if(text.split(from).length!==2)throw Error('Pinned account database boundary changed: '+from);return text.replace(from,to);};
export function prepareProcessDatabase({bionicSource,coreSource,buildSource,python,build}){
 const output=path.join(build,'process-database'),source=path.join(output,'source'),include=path.join(output,'include');
 exportSources(bionicSource,pins.aospBionic,['libc/bionic/grp_pwd.cpp','libc/bionic/grp_pwd_file.cpp','libc/bionic/grp_pwd_file.h','libc/private/grp_pwd.h','libc/private/ErrnoRestorer.h','libc/private/ScopedFd.h','libc/platform/bionic/macros.h','libc/include/pwd.h','libc/include/grp.h','libc/include/sys/cdefs.h','libc/libc.map.txt','libc/NOTICE'],source);
 exportSources(coreSource,pins.aospSystemCore,['libcutils/include/private/android_filesystem_config.h','libcutils/NOTICE'],path.join(output,'core'));
 exportSources(buildSource,catalog.find(x=>x.name==='build').revision,['tools/fs_config/fs_config_generator.py'],path.join(output,'build'));
 fs.mkdirSync(path.join(include,'private'),{recursive:true});fs.mkdirSync(path.join(include,'async_safe'),{recursive:true});
 const ids=execFileSync(python,[path.join(output,'build/tools/fs_config/fs_config_generator.py'),'aidarray',path.join(output,'core/libcutils/include/private/android_filesystem_config.h')],{encoding:'utf8'});
 fs.writeFileSync(path.join(include,'generated_android_ids.h'),ids);
 fs.writeFileSync(path.join(include,'private/bionic_lock.h'),'#pragma once\n#include <mutex>\nclass Lock {std::mutex value;public:void init(bool){} void lock(){value.lock();}void unlock(){value.unlock();}};\nclass LockGuard {Lock& value;public:explicit LockGuard(Lock& lock):value(lock){value.lock();}~LockGuard(){value.unlock();}};\n');
 fs.writeFileSync(path.join(include,'async_safe/log.h'),'#pragma once\n#include <stdio.h>\n#include <stdarg.h>\n#define ANDROID_LOG_ERROR 6\nstatic inline void async_safe_format_log(int,const char*tag,const char*format,...){fprintf(stderr,"[%s] ",tag);va_list args;va_start(args,format);vfprintf(stderr,format,args);va_end(args);fputc(10,stderr);}\n');
 let main=fs.readFileSync(path.join(source,'libc/bionic/grp_pwd.cpp'),'utf8');
 const exports=new Set([...fs.readFileSync(path.join(source,'libc/libc.map.txt'),'utf8').matchAll(/^\s*(\w+);/gm)].map(m=>m[1]));
 const declarations=[...main.matchAll(/^(?:passwd\*|group\*|int|void|char\*)\s+(\w+)\([^{}]*\)\s*\{/gm)].filter(m=>exports.has(m[1])).map(m=>({name:m[1],declaration:m[0].slice(0,-1).trim()+';'}));
 if(declarations.length<15)throw Error('Original account database exports changed');
 const structures=['pwd','grp'].map(name=>{const text=fs.readFileSync(path.join(source,'libc/include/'+name+'.h'),'utf8');return text.match(/struct (?:passwd|group) \{[\s\S]*?\n\};/)[0];}).join('\n');
 const alignment=fs.readFileSync(path.join(source,'libc/include/sys/cdefs.h'),'utf8').match(/^#define __BIONIC_ALIGN\([^\n]+/m)[0];
 const prefix=`#pragma once
#include <cstdint>
#include <cstddef>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <grp.h>
#include <pwd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <limits.h>
#include ${JSON.stringify(path.join(root,'process-identity.h'))}
extern "C" const char *kl_guest_path(const char *,char *,size_t);
extern "C" void *kl_shim_lookup(const char *);
extern "C" int klj_process_setgroups(size_t,const gid_t *);
static inline int klj_database_open(const char*file,int flags){char resolved[PATH_MAX];return open(kl_guest_path(file,resolved,sizeof resolved),flags);}
static inline int klj_database_property(const char*key,char*value){auto get=(int(*)(const char*,char*))kl_shim_lookup("__system_property_get");return get(key,value);}
#define PROP_VALUE_MAX 92
#define __system_property_get klj_database_property
#define getuid klj_process_uid
#define setgroups klj_process_setgroups
#define passwd klj_bionic_passwd
#define group klj_bionic_group
${alignment}
${structures}
${declarations.map(x=>'#define '+x.name+' klj_bionic_'+x.name).join('\n')}
extern "C" {\n${declarations.map(x=>x.declaration).join('\n')}\n}
#define __ANDROID__ 1
`;
 fs.writeFileSync(path.join(include,'database.h'),prefix);
 for(const header of ['android/api-level.h','sys/system_properties.h','mntent.h'])main=replace(main,'#include <'+header+'>','');
 main=replace(main,'#include "bionic/pthread_internal.h"','static thread_local struct {group_state_t group;passwd_state_t passwd;} klj_database_tls;\nstatic auto& __get_bionic_tls(){return klj_database_tls;}');
 fs.writeFileSync(path.join(output,'grp_pwd.cpp'),main);
 let files=fs.readFileSync(path.join(source,'libc/bionic/grp_pwd_file.cpp'),'utf8');files=replace(files,'ScopedFd fd(open(filename_, O_CLOEXEC | O_NOFOLLOW | O_RDONLY));','ScopedFd fd(klj_database_open(filename_, O_CLOEXEC | O_NOFOLLOW | O_RDONLY));');
 fs.writeFileSync(path.join(output,'grp_pwd_file.cpp'),files);
 const flags=['-std=c++20','-arch','arm64','-mmacosx-version-min=13.0','-O2','-fPIC','-Wall','-Wextra','-Wno-unused-function','-Wno-nullability-completeness','-I'+include,'-I'+path.join(source,'libc'),'-I'+path.join(source,'libc/bionic'),'-I'+path.join(output,'core/libcutils/include'),'-include',path.join(include,'database.h')];
 // Resolve the entire original module before Klepton's host-forwarding table:
 // Darwin passwd/group layouts differ even where the function names match.
 fs.writeFileSync(path.join(output,'lookup.cpp'),'extern "C" int klj_database_name_id(const char *name,int group_lookup){\nif(group_lookup){auto *value=getgrnam(name);return value?static_cast<int>(value->gr_gid):-1;}\nauto *value=getpwnam(name);return value?static_cast<int>(value->pw_uid):-1;\n}\nextern "C" void *klj_database_lookup(const char *name){\n'+declarations.map(x=>'if(!strcmp(name,"'+x.name+'"))return reinterpret_cast<void*>(&klj_bionic_'+x.name+');').join('\n')+'\nreturn nullptr;\n}\n');
 const objects=['grp_pwd','grp_pwd_file','lookup'].map(name=>{const object=path.join(output,name+'.o');execFileSync('clang++',[...flags,'-c',path.join(output,name+'.cpp'),'-o',object],{stdio:'inherit'});return object;});
 const notices=path.join(output,'notices');fs.mkdirSync(notices,{recursive:true});copyOptional(path.join(source,'libc/NOTICE'),path.join(notices,'NOTICE'));
 copyOptional(path.join(output,'core/libcutils/NOTICE'),path.join(notices,'APACHE-2.0.txt'));
 return {objects,notices,functions:declarations.map(x=>x.name)};
}
