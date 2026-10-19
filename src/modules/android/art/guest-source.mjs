import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {exportSources} from '../native/binder-rpc/build.mjs';
import {prepareRpcIdentity,prepareRpcSessionOwnership} from '../native/binder-rpc/rpc-sources.mjs';
import {prepareSourceTree,copyChanged,writeChanged} from '../native/upstream/source-files.mjs';
import pins from '../native/source-pins.json' with {type:'json'};

const root=import.meta.dirname,shared=path.resolve(root,'../native/binder-rpc');
function replace(text,before,after){if(text.split(before).length!==2)throw Error('Android ART Binder boundary changed: '+before);return text.replace(before,after);}
function adapt(file,edit){writeChanged(file,edit(fs.readFileSync(file,'utf8')));}

/** Keep the matching original kernel Binder and RPC class representation.
 * The complete existing JNI/identity owner is shared with the JVM supplier;
 * only credentials, JNI object conversion and the wire envelope differ. */
export function prepareArtBinderSource({nativeSource,output}){
 const relativeSources=[];
 prepareSourceTree(output,temporary=>{
  const binder=path.join(temporary,'binder'),generated=path.join(temporary,'adapter');
  exportSources(nativeSource,pins.aospNative,['libs/binder'],path.join(temporary,'original'));
  fs.renameSync(path.join(temporary,'original/libs/binder'),binder);fs.rmSync(path.join(temporary,'original'),{recursive:true});fs.mkdirSync(generated,{recursive:true});
  for(const name of ['ParcelTransport.cpp','ParcelTransport.h','RpcSessionBridge.cpp','rpc-identity.h','descriptor-capability.h'])copyChanged(path.join(shared,name),path.join(generated,name));
  copyChanged(path.join(root,'native/rpc-jni.h'),path.join(generated,'rpc-jni.h'));
  for(const name of ['binder-ParcelTransport.cpp.patch','binder-ParcelTransport.h.patch'])execFileSync('git',['apply','--unsafe-paths',path.join(root,'adapters',name)],{cwd:generated,stdio:'inherit'});
  prepareRpcSessionOwnership(binder);
  adapt(path.join(binder,'Binder.cpp'),text=>{
   // A native game thread can reach a local JavaBBinder without entering an
   // RPC worker. Keep the exact original endpoint/Parcel dispatch and adopt
   // that caller through the existing VM attachment owner before it runs.
   const dispatch='status_t BBinder::transact(\n    uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)\n{';
   return 'extern "C" void klj_art_rpc_bind_native_thread();\n'+replace(text,dispatch,dispatch+'\n    klj_art_rpc_bind_native_thread();');
  });
  const identity=[];prepareRpcIdentity({root:generated,binder,generated,sources:identity});
  relativeSources.push(...identity.map(file=>path.relative(temporary,file)),path.relative(temporary,path.join(generated,'ParcelTransport.cpp')));
  adapt(path.join(binder,'include/binder/Parcel.h'),text=>replace(text,'    friend class RpcState;','    friend class RpcState;\n    friend class HostParcelTransport;'));
  adapt(path.join(binder,'include/binder/Stability.h'),text=>replace(replace(text,'class BpBinder;','class BpBinder;\nclass HostParcelTransport;'),'friend ::android::Parcel;','friend ::android::Parcel;\n    friend ::android::HostParcelTransport;'));
  adapt(path.join(binder,'Parcel.cpp'),text=>{
   // Original android_runtime JNI always writes the kernel Parcel object
   // representation. The shared envelope binds the RPC wire format once at
   // transact, after translating all Binder/descriptor capabilities.
   text=replace(text,`    if (binder && binder->remoteBinder() && binder->remoteBinder()->isRpcBinder()) {
        markForRpc(binder->remoteBinder()->getPrivateAccessor().rpcSession());
    }`,`    (void)binder; // The native proxy binds the transport when sending.`);
   text=replace(text,'header != kHeader && !mServiceFuzzing',"(header != kHeader && !(art_rpc_from_host() && header == B_PACK_CHARS('U','N','K','N'))) && !mServiceFuzzing");
   text=replace(text,'status_t Parcel::appendFrom(const Parcel* parcel, size_t offset, size_t len) {','status_t Parcel::appendFrom(const Parcel* parcel, size_t offset, size_t len) {\n    if (!mData && !mDataSize && !mDataPos && !isForRpc() && parcel->isForRpc()) markForRpc(parcel->maybeRpcFields()->mSession);');
   text=replace(text,`                if (proxy->isRpcBinder()) {
                    ALOGE("Sending a socket binder over kernel binder is prohibited");
                    return INVALID_OPERATION;
                }`,`                if (proxy->isRpcBinder()) {
                    obj.hdr.type = BINDER_TYPE_BINDER;
                    obj.flags = 0;
                    obj.binder = reinterpret_cast<uintptr_t>(binder->getWeakRefs());
                    obj.cookie = reinterpret_cast<uintptr_t>(binder.get());
                    status_t status = writeObject(obj, false);
                    return status == OK ? finishFlattenBinder(binder) : status;
                }`);
   text=replace(text,'status_t Parcel::flattenBinder(const sp<IBinder>& binder) {','status_t Parcel::flattenBinder(const sp<IBinder>& original) {\n    const sp<IBinder> binder = isForRpc() ? HostParcelTransport::exportBinder(original,maybeRpcFields()->mSession) : original;');
   return '#include "ParcelTransport.h"\nextern "C" bool art_rpc_from_host();\n'+text;
  });
  // Upstream translation units define LOG_TAG before logging headers. Preserve
  // that ordering when adding the transport dependency.
  for(const [unit,header]of [['BpBinder','ParcelTransport.h'],['Parcel','ParcelTransport.h'],['RpcState','rpc-identity.h'],['RpcServer','rpc-identity.h'],['RpcSession','rpc-identity.h']])adapt(path.join(binder,unit+'.cpp'),text=>{
   const include='#include "'+header+'"\n';text=replace(text,include,'');
   const tags=[...text.matchAll(/^#define LOG_TAG [^\n]+$/gm)];if(tags.length!==1)throw Error('Original Binder logging owner changed');
   return replace(text,tags[0][0],tags[0][0]+'\n'+include);
  });
  adapt(path.join(generated,'RpcSessionBridge.cpp'),text=>{
   text=text.replace('#include "hotspot-jni.h"','#include "rpc-jni.h"').replace('#include "android_util_Binder.h"','').replace('#include <android_runtime/AndroidRuntime.h>','');
   text=replace(text,`        pid_t pid=0;uid_t uid=0;gid_t gid=0;socklen_t size=sizeof(pid);
        if(getsockopt(socket,SOL_LOCAL,LOCAL_PEERPID,&pid,&size)<0||size!=sizeof(pid)||
            getpeereid(socket,&uid,&gid)<0||uid!=getuid())return {};`,`        KljArtPeerCredentials credentials{.descriptor=socket};
        if(klj_art_host(12,&credentials,sizeof(credentials))<0||
            credentials.hostUid!=artHostUid.load(std::memory_order_relaxed))return {};
        const pid_t pid=credentials.androidPid;`);
   text=text.replace('int kljRegisterBinderRpc(JNIEnv* env)','__attribute__((visibility("default"))) int kljRegisterBinderRpc(JNIEnv* env)');
   text=text.replaceAll('AndroidRuntime::getJNIEnv()','artJniEnvironment()').replaceAll('android::ibinderForJavaObject','artBinderForJavaObject').replaceAll('=ibinderForJavaObject(','=artBinderForJavaObject(').replaceAll('return javaObjectForIBinder(','return artJavaObjectForBinder(').replace('address.sun_len=sizeof(address);','');
   return text+`\n// Native relay enrollment uses the same immutable peer leases as Java RPC.
extern "C" __attribute__((visibility("default"))) bool art_rpc_from_host(){return callingPeer!=nullptr;}
extern "C" __attribute__((visibility("default"))) int klj_art_rpc_peer(void* session,int pid,unsigned uid,const char* sid,const char* packageName,const char* processName){
    if(!session||pid<=0||!sid||!packageName||!processName)return android::BAD_VALUE;
    auto peer=std::make_shared<Peer>();peer->pid=pid;peer->uid=uid;peer->securityContext=sid;peer->packageName=packageName;peer->processName=processName;
    std::lock_guard guard(peersMutex);return peers.emplace(static_cast<android::RpcSession*>(session),std::move(peer)).second?android::OK:android::ALREADY_EXISTS;
}
`;
  });
 });
 return {binder:path.join(output,'binder'),adapter:path.join(output,'adapter'),sources:relativeSources.map(file=>path.join(output,file))};
}
