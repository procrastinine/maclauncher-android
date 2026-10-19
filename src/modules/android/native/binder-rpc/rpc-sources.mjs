import fs from 'node:fs';
import path from 'node:path';
import {includeDependencies} from '../upstream/source-includes.mjs';

function replace(text,before,after){if(text.split(before).length!==2)throw Error('AOSP Binder RPC boundary changed: '+before);return text.replace(before,after);}

export function prepareRpcSessionOwnership(binder){
  const file=path.join(binder,'RpcSession.cpp'),text=fs.readFileSync(file,'utf8');
  // The waiting caller owns joinCv on its stack. Once the worker unlocks,
  // a predicate/spurious wake can destroy it before notify_one completes.
  // Publish the wake while holding the same mutex, so notification finishes
  // before the caller can retire its stack state. Keep upstream thread/session
  // ownership, connection setup, and dispatch unchanged.
  fs.writeFileSync(file,replace(text,
    '        ownershipTransferred = true;\n        threadLock.unlock();\n        joinCv.notify_one();',
    '        ownershipTransferred = true;\n        joinCv.notify_one();\n        threadLock.unlock();'));

  // Start returns with the original shutdown trigger installed. The original
  // RpcServer then owns both thread startup and retirement; immediate close
  // cannot race a join thread that has not initialized its trigger yet.
  const serverOwner=path.join(binder,'RpcServer.cpp');
  let lifecycle=fs.readFileSync(serverOwner,'utf8');
  lifecycle=replace(lifecycle,'void RpcServer::start() {\n    RpcMutexLockGuard _l(mLock);',
    'void RpcServer::start() {\n    RpcMutexUniqueLock _l(mLock);');
  lifecycle=replace(lifecycle,'    rpcJoinIfSingleThreaded(*mJoinThread);\n}',
    '    rpcJoinIfSingleThreaded(*mJoinThread);\n    if constexpr (kEnableRpcThreads) mShutdownCv.wait(_l, [this] { return mShutdownTrigger != nullptr; });\n}');
  lifecycle=replace(lifecycle,'        LOG_ALWAYS_FATAL_IF(mShutdownTrigger == nullptr, "Cannot create join signaler");\n    }',
    '        LOG_ALWAYS_FATAL_IF(mShutdownTrigger == nullptr, "Cannot create join signaler");\n    }\n    mShutdownCv.notify_all();');
  fs.writeFileSync(serverOwner,lifecycle);

  // Darwin may disconnect a Unix socket while its descriptor is still queued
  // in SCM_RIGHTS. The bootstrap owns each sending reference until the server
  // confirms recvmsg has installed the receiving reference. This is only the
  // platform bootstrap envelope; original Binder connection/payload ownership
  // and protocol stay unchanged. Both waits use the existing shutdown trigger.
  const session=fs.readFileSync(file,'utf8');
  fs.writeFileSync(file,replace(session,
    '        return initAndAddConnection(RpcTransportFd(std::move(clientFd)), sessionId, incoming);',
    `        uint8_t accepted = 0;
        iovec receipt{&accepted, sizeof(accepted)};
        status = mBootstrapTransport->interruptableReadFully(mShutdownTrigger.get(), &receipt,
                                                             1, std::nullopt, nullptr);
        if (status != OK) return status;
        if (accepted != 1) return static_cast<status_t>(BAD_VALUE);
        return initAndAddConnection(RpcTransportFd(std::move(clientFd)), sessionId, incoming);`));
  const server=path.join(binder,'RpcServer.cpp');
  fs.writeFileSync(server,replace(fs.readFileSync(server,'utf8'),
    '    unique_fd fd(std::move(std::get<unique_fd>(fds.back())));\n    if (status_t res = binder::os::setNonBlocking(fd); res != OK) return res;',
    `    unique_fd fd(std::move(std::get<unique_fd>(fds.back())));
    if (status_t res = binder::os::setNonBlocking(fd); res != OK) return res;
    uint8_t accepted = 1;
    iovec receipt{&accepted, sizeof(accepted)};
    for (;;) {
        ssize_t sent = binder::os::sendMessageOnSocket(server.mServer, &receipt, 1, nullptr);
        if (sent == 1) break;
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            status_t status = server.mShutdownTrigger->triggerablePoll(server.mServer, POLLOUT);
            if (status != OK) return status;
            continue;
        }
        return sent == 0 ? DEAD_OBJECT : -errno;
    }`));
}

/** Attach authenticated host-process identity at the actual RPC dispatch. */
export function prepareRpcIdentity({root,binder,generated,sources}){
  const transportHeader=path.join(binder,'include/binder/RpcTransport.h');
  let transport=fs.readFileSync(transportHeader,'utf8');
  transport=replace(transport,'class RpcTransportCtxRaw;','class RpcTransportCtxRaw;\nclass HostAuthenticatedTransportCtx;');
  transport=replace(transport,'    friend class ::android::RpcTransportCtxRaw;','    friend class ::android::RpcTransportCtxRaw;\n    friend class ::android::HostAuthenticatedTransportCtx;');
  fs.writeFileSync(transportHeader,transport);
  const proxyHeader=path.join(binder,'include/binder/BpBinder.h');
  fs.writeFileSync(proxyHeader,replace(fs.readFileSync(proxyHeader,'utf8'),'class BpBinder : public IBinder {\npublic:',
    'class BpBinder : public IBinder {\npublic:\n    const sp<RpcSession>& hostRpcSession() const { return rpcSession(); }'));
  // Original native IBinder methods also construct local-format Parcels.
  // Select the existing shared envelope at this proxy boundary for both VMs;
  // an already encoded RPC Parcel follows the original session directly.
  const proxy=path.join(binder,'BpBinder.cpp');
  fs.writeFileSync(proxy,includeDependencies(replace(fs.readFileSync(proxy,'utf8'),
    'status = rpcSession()->transact(sp<IBinder>::fromExisting(this), code, data, reply,\n                                            flags);',
    'status = data.isForRpc() ? rpcSession()->transact(sp<IBinder>::fromExisting(this), code, data, reply, flags) : HostParcelTransport::transact(this,code,data,reply,flags);'),
    '#include "ParcelTransport.h"'));

  const header=path.join(binder,'include/binder/IPCThreadState.h');
  fs.writeFileSync(header,replace(fs.readFileSync(header,'utf8'),'class IPCThreadState {\npublic:','class IPCThreadState {\npublic:\n    status_t transactRpcWithIdentity(pid_t,uid_t,const char*,const sp<IBinder>&,uint32_t,const Parcel&,Parcel*,uint32_t);'));
  const state=fs.readFileSync(path.join(binder,'IPCThreadState.cpp'),'utf8');
  const begin=state.indexOf('            const void* origServingStackPointer = mServingStackPointer;');
  const end=state.indexOf('            // ALOGI(">>>> TRANSACT',begin);
  const restoreBegin=state.indexOf('            mServingStackPointer = origServingStackPointer;',end);
  const restoreEnd=state.indexOf('\n            IF_LOG_TRANSACTIONS()',restoreBegin);
  if(begin<0||end<begin||restoreBegin<end||restoreEnd<restoreBegin)throw Error('AOSP Binder calling-identity ownership changed');
  let enter=state.slice(begin,end);
  enter=replace(enter,'tr.sender_pid','pid');enter=replace(enter,'reinterpret_cast<const char*>(tr_secctx.secctx)','sid');enter=replace(enter,'tr.sender_euid','uid');enter=replace(enter,'tr.flags','flags');
  const restore=state.slice(restoreBegin,restoreEnd);
  // Reuse the complete original kernel-dispatch identity/work-source save and
  // restore operations, supplied with the supervisor's authenticated peer.
  const identity=path.join(generated,'RpcCallingIdentity.cpp');
  fs.writeFileSync(identity,state.slice(0,state.indexOf('#define LOG_TAG'))+'#include <binder/IPCThreadState.h>\n#include <binder/IBinder.h>\n#include <binder/Functional.h>\nnamespace android {\nstatus_t IPCThreadState::transactRpcWithIdentity(pid_t pid,uid_t uid,const char* sid,const sp<IBinder>& target,uint32_t code,const Parcel& data,Parcel* reply,uint32_t flags){\n'+enter+'\n    auto restore=binder::impl::make_scope_guard([&]{\n'+restore+'\n    });\n    return target->transact(code,data,reply,flags);\n}\n}\n');
  sources.push(identity,path.join(root,'RpcSessionBridge.cpp'));
  const rpcState=path.join(binder,'RpcState.cpp');
  fs.writeFileSync(rpcState,'#include "rpc-identity.h"\n'+replace(fs.readFileSync(rpcState,'utf8'),'replyStatus = target->transact(transaction->code, data, &reply, transaction->flags);','replyStatus = klj_rpc_transact(session,target,transaction->code,data,&reply,transaction->flags);'));
  const serverIdentity=path.join(binder,'RpcServer.cpp');
  fs.writeFileSync(serverIdentity,'#include "rpc-identity.h"\n'+replace(fs.readFileSync(serverIdentity,'utf8'),
    '        if (incoming) {\n            LOG_ALWAYS_FATAL_IF(OK != session->addOutgoingConnection',
    '        if (!klj_rpc_authorize_connection(session)) return;\n        if (incoming) {\n            LOG_ALWAYS_FATAL_IF(OK != session->addOutgoingConnection'));
  const session=path.join(binder,'RpcSession.cpp');
  fs.writeFileSync(session,'#include "rpc-identity.h"\n'+replace(fs.readFileSync(session,'utf8'),'RpcSession::~RpcSession() {','RpcSession::~RpcSession() {\n    klj_rpc_forget(this);'));
}
