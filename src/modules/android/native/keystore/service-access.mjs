import fs from 'node:fs';import path from 'node:path';import{execFileSync}from'node:child_process';
export function prepareServiceAccess({output:out,source:repo,revision}) {
fs.mkdirSync(out,{recursive:true});
for(const file of ['Access.h','Access.cpp']){
 let text=execFileSync('git',['-C',repo,'show',revision+':cmds/servicemanager/'+file],{encoding:'utf8'});
 if(file==='Access.cpp'){
  text=text.replaceAll('#ifdef __ANDROID__','#if defined(__ANDROID__) || defined(KL_HOST_SERVICE_ACCESS)');
  // The directory is a logical service-manager object hosted by the KeyStore
  // process; peer process SIDs remain those authenticated by original Binder.
  text=text.replace('CHECK(getcon(&mThisProcessContext) == 0);','CHECK(klj_service_directory_context(&mThisProcessContext) == 0);');
  text='extern "C" int klj_service_directory_context(char**);\n'+text;
 }
 fs.writeFileSync(out+'/'+file,text);
}

return {source:out+'/Access.cpp',include:out};
}
