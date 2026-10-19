import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {parseBlueprint} from '../upstream/blueprint.mjs';

/** Original attestation application-ID encoding and declared AIDL types. */
export function buildAttestationIds({framework,security,compiler:aidl,output:out,keyStoreAidl:ks,keyMintAidl,wrapper,binder}) {
fs.mkdirSync(out,{recursive:true});
const definition=framework+'/keystore/aaid/aidl',module=parseBlueprint(fs.readFileSync(definition+'/Android.bp','utf8')).modules.find(m=>m.type==='aidl_interface').properties;
const inputs=fs.globSync(module.srcs,{cwd:definition}).sort().map(f=>definition+'/'+f);
const generated=out+'/generated';fs.mkdirSync(generated,{recursive:true});
execFileSync(aidl,['--lang=cpp','--structured','--min_sdk_version=31','--omit_invocation','-I'+definition,'-h',generated+'/include','-o',generated,...inputs],{stdio:'inherit'});
const keymint=keyMintAidl.records.find(m=>m.name==='android.hardware.security.keymint');
const ndk=out+'/ndk';fs.mkdirSync(ndk,{recursive:true});
execFileSync(aidl,['--lang=ndk','--structured','--min_sdk_version=31','--omit_invocation','--stability=vintf','--version='+ks.version,'--hash='+ks.hash,'-I'+ks.snapshot,'-I'+keymint.snapshot,'-h',ndk+'/include','-o',ndk,...ks.inputs.map(i=>i.file)],{stdio:'inherit'});
const aaid=security+'/keystore2/aaid';
const baseFlags=wrapper.flags;
const flags=[...baseFlags,'-I'+generated+'/include','-I'+ndk+'/include','-I'+security+'/keystore/include','-I'+binder.output+'/service-aidl/include','-I'+binder.output+'/source/core/libcutils/include','-I'+binder.output+'/source/core/include','-I'+aaid];
const sources=[security+'/keystore/keystore_attestation_id.cpp',aaid+'/aaid.cpp',...fs.globSync('**/*.cpp',{cwd:generated}).sort().map(f=>generated+'/'+f)];
const objects=[];for(const [i,file] of sources.entries()){const object=out+'/'+i+'.o';execFileSync('clang++',[...flags,'-c',file,'-o',object],{stdio:'inherit'});objects.push(object);}
const archive=out+'/libkeystore2-aaid.a';fs.rmSync(archive,{force:true});execFileSync('ar',['rcs',archive,...objects]);
fs.writeFileSync(out+'/closure.json',JSON.stringify({module:module.name,inputs,sources,flags,archive,originalAttestationEncoding:true},null,2)+'\n');
return {archive,sources,flags};
}
