import fs from 'node:fs';import path from 'node:path';import{fileURLToPath}from'node:url';import{execFileSync}from'node:child_process';
import{parseBlueprint}from'../upstream/blueprint.mjs';
const adapter=path.dirname(fileURLToPath(import.meta.url));
export function prepareNativeHal({output:root,security,hardware,compiler,keyMintAidl}) {
const s=path.join(security,'keystore2'),o=root+'/overlay';
fs.mkdirSync(o,{recursive:true});
for(const file of ['PlatformHal.cpp','PlatformHal.h'])fs.copyFileSync(path.join(adapter,file),path.join(o,file));
function replace(text,from,to){if(!text.includes(from))throw Error('missing adaptation anchor '+from);return text.replace(from,to)}
function put(file,text){fs.writeFileSync(o+'/'+file,text)}
let apc=fs.readFileSync(s+'/apc_compat/apc_compat.cpp','utf8');
apc=replace(apc,'#include <android/hardware/confirmationui/1.0/IConfirmationUI.h>','#if !defined(__APPLE__)\n#include <android/hardware/confirmationui/1.0/IConfirmationUI.h>');
apc=replace(apc,'#include <hwbinder/IBinder.h>','#include <hwbinder/IBinder.h>\n#endif\n#include <cstring>\n#include <mutex>\n#include <optional>');
apc=replace(apc,'using android::sp;','#if !defined(__APPLE__)\nusing android::sp;');
apc=replace(apc,'using AidlConfirmationUI =','#endif\n\nusing AidlConfirmationUI =');
apc=replace(apc,'class ConfuiHidlCompatSession :','#if !defined(__APPLE__)\nclass ConfuiHidlCompatSession :');
apc=replace(apc,'class ConfuiAidlCompatSession :','#endif\n\nclass ConfuiAidlCompatSession :');
apc=replace(apc,'        sp<ConfuiHidlCompatSession> hidlCompatSession =','#if !defined(__APPLE__)\n        sp<ConfuiHidlCompatSession> hidlCompatSession =');
apc=replace(apc,'        LOG(ERROR) << "ConfirmationUI: Not found Service";','#endif\n        LOG(ERROR) << "ConfirmationUI: Not found Service";');
apc=replace(apc,'            return hidlCompatSession_->promptUserConfirmation(callback, prompt_text, extra_data,\n                                                              extra_data_size, locale, ui_options);','#if !defined(__APPLE__)\n            return hidlCompatSession_->promptUserConfirmation(callback, prompt_text, extra_data,\n                                                              extra_data_size, locale, ui_options);\n#else\n            return APC_COMPAT_ERROR_SYSTEM_ERROR;\n#endif');
apc=replace(apc,'            return hidlCompatSession_->abort();','#if !defined(__APPLE__)\n            return hidlCompatSession_->abort();\n#endif');
apc=replace(apc,'    ApcCompatSession(std::shared_ptr<ConfuiAidlCompatSession> aidlCompatSession,','#if defined(__APPLE__)\n    ApcCompatSession(std::shared_ptr<ConfuiAidlCompatSession> aidlCompatSession, std::nullptr_t)\n        : aidlCompatSession_(std::move(aidlCompatSession)) {}\n#else\n    ApcCompatSession(std::shared_ptr<ConfuiAidlCompatSession> aidlCompatSession,');
apc=replace(apc,'        : aidlCompatSession_(aidlCompatSession), hidlCompatSession_(hidlCompatSession) {}','        : aidlCompatSession_(aidlCompatSession), hidlCompatSession_(hidlCompatSession) {}\n#endif');
apc=replace(apc,'    sp<ConfuiHidlCompatSession> hidlCompatSession_;','#if !defined(__APPLE__)\n    sp<ConfuiHidlCompatSession> hidlCompatSession_;\n#endif');put('apc_compat.cpp',apc);fs.copyFileSync(s+'/apc_compat/apc_compat.hpp',o+'/apc_compat.hpp');
let hal=fs.readFileSync(s+'/src/hal_instance_names/hal_names.cpp','utf8');
hal=replace(hal,'#include <hidl/ServiceManagement.h>','#if !defined(__APPLE__)\n#include <hidl/ServiceManagement.h>\n#endif\n#include <algorithm>\n#include <iterator>\n#include <string>\n#include <vector>');
hal=replace(hal,'    const auto halNames = android::hardware::getAllHalInstanceNames(factoryName);','#if defined(__APPLE__)\n    // This platform exposes no physical Android HIDL devices.\n    const std::vector<std::string> halNames;\n#else\n    const auto halNames = android::hardware::getAllHalInstanceNames(factoryName);\n#endif');put('hal_names.cpp',hal);fs.copyFileSync(s+'/src/hal_instance_names/hal_names.hpp',o+'/hal_names.hpp');
let h=fs.readFileSync(s+'/src/km_compat/km_compat.h','utf8');
h=replace(h,'#include <keymasterV4_1/Keymaster4.h>','#if !defined(__APPLE__)\n#include <keymasterV4_1/Keymaster4.h>\n#endif');
h=replace(h,'#include "certificate_utils.h"','#if !defined(__APPLE__)\n#include "certificate_utils.h"\n#endif');
for(const t of ['using V4_0_ErrorCode = ::android::hardware::keymaster::V4_0::ErrorCode;','using ::android::hardware::keymaster::V4_1::support::Keymaster;'])h=replace(h,t,'#if !defined(__APPLE__)\n'+t+'\n#endif');
h=replace(h,'class OperationSlot;','#if !defined(__APPLE__)\nclass OperationSlot;');h=replace(h,'class KeystoreCompatService :','#endif\n\nclass KeystoreCompatService :');put('km_compat.h',h);
let km=fs.readFileSync(s+'/src/km_compat/km_compat.cpp','utf8');
km=replace(km,'#include "km_compat_type_conversion.h"','#if !defined(__APPLE__)\n#include "km_compat_type_conversion.h"');
km=replace(km,'ScopedAStatus\nKeystoreCompatService::getKeyMintDevice','#else\n#include "PlatformHal.h"\n#endif\n\nScopedAStatus\nKeystoreCompatService::getKeyMintDevice');
for(const [old,next]of[['KeyMintDevice::createKeyMintDevice','klepton::keystore::keyMintDevice'],['SharedSecret::createSharedSecret','klepton::keystore::legacySharedSecret'],['SecureClock::createSecureClock','klepton::keystore::legacySecureClock']]){
const tail=km.indexOf('ScopedAStatus\nKeystoreCompatService::getKeyMintDevice');const prefix=km.slice(0,tail);let body=km.slice(tail);const line=body.split('\n').find(l=>l.includes(old));body=replace(body,line,'#if defined(__APPLE__)\n'+line.replace(old,next)+'\n#else\n'+line+'\n#endif');km=prefix+body;}
put('km_compat.cpp',km);fs.copyFileSync(s+'/src/km_compat/km_compat_service.cpp',o+'/km_compat_service.cpp');
const deps=keyMintAidl.records;
const modules=[];
for(const [directory,name]of[[hardware+'/confirmationui/aidl','android.hardware.confirmationui'],[s+'/aidl','android.security.compat']]){
 const p=parseBlueprint(fs.readFileSync(directory+'/Android.bp','utf8')).modules.find(m=>m.properties.name===name).properties;
 const version=p.versions_with_info?.at(-1)?.version;const snapshot=version?directory+'/aidl_api/'+name+'/'+version:directory;
 const files=fs.globSync(p.srcs,{cwd:snapshot}).sort().map(f=>snapshot+'/'+f);if(!files.length)throw Error(name);
 const dest=root+'/generated/'+name;fs.mkdirSync(dest,{recursive:true});
 const args=['--lang=ndk','--structured',...(p.stability?['--stability='+p.stability]:[]),...(version?['--version='+version,'--hash='+fs.readFileSync(snapshot+'/.hash','utf8').trim().split(/\s+/).at(-1)]:[]),'--min_sdk_version=35','--omit_invocation','-a','-I',snapshot,...deps.flatMap(d=>['-I',d.snapshot]),'-o',dest,'-h',dest+'/include',...files];
 execFileSync(compiler,args,{stdio:'inherit'});modules.push({name,properties:p,snapshot,files,args,output:dest});
}
fs.writeFileSync(root+'/generation.json',JSON.stringify(modules,null,2)+'\n');
return {output:root,overlay:o,modules};
}
