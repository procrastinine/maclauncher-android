import {includeDependencies} from '../upstream/source-includes.mjs';

function replace(source,before,after){
  if(source.split(before).length!==2)throw Error('Original Binder process transport changed: '+before);
  return source.replace(before,after);
}
function body(source,signature,replacement){
  const start=source.indexOf(signature),open=source.indexOf('{',start),end=source.indexOf('\n}',open);
  if(start<0||open<start||end<open||source.indexOf(signature,start+1)>=0)throw Error('Original Binder process operation changed: '+signature);
  return source.slice(0,open+1)+'\n'+replacement+'\n'+source.slice(end);
}

/** The existing RpcSession owns every worker and connection. ProcessState's
 * native thread-pool controls change that transport, without a kernel pool. */
export function adaptRpcPoolSource(file,source){
  if(file.endsWith('RpcSession.h')){
    source=replace(source,'    LIBBINDER_EXPORTED void setMaxIncomingThreads(size_t threads);',
      '    LIBBINDER_EXPORTED void setMaxIncomingThreads(size_t threads);\n    LIBBINDER_EXPORTED status_t growIncomingThreadPool(size_t threads);\n    void setHostLocalBridge() { mHostLocalBridge = true; }\n    bool isHostLocalBridge() const { return mHostLocalBridge; }');
    source=replace(source,'    std::unique_ptr<RpcTransport> mBootstrapTransport;',
      '    std::unique_ptr<RpcTransport> mBootstrapTransport;\n    bool mHostLocalBridge = false;\n    RpcMutex mBootstrapMutex;\n    RpcMutex mIncomingGrowthMutex;\n    status_t setupBootstrapConnection(const std::vector<uint8_t>& sessionId, bool incoming);');
    return source;
  }
  if(!file.endsWith('RpcSession.cpp'))throw Error('Unexpected original RPC pool source: '+file);
  const start=source.indexOf('status_t RpcSession::setupUnixDomainSocketBootstrapClient(');
  const lambda=source.indexOf('    return setupClient([&](const std::vector<uint8_t>& sessionId, bool incoming) {',start);
  const end=source.indexOf('\n    });\n}',lambda);
  if(start<0||lambda<start||end<lambda)throw Error('Original RPC bootstrap connection operation changed');
  const operation=source.slice(source.indexOf('\n',lambda)+1,end);
  source=source.slice(0,lambda)+`    return setupClient([&](const std::vector<uint8_t>& sessionId, bool incoming) {
        return setupBootstrapConnection(sessionId, incoming);
    });
}

status_t RpcSession::setupBootstrapConnection(const std::vector<uint8_t>& sessionId, bool incoming) {
    RpcMutexLockGuard bootstrap(mBootstrapMutex);
${operation}
}

status_t RpcSession::growIncomingThreadPool(size_t threads) {
    RpcMutexLockGuard growth(mIncomingGrowthMutex);
    size_t previous;
    {
        RpcMutexLockGuard lock(mMutex);
        if (!mStartedSetup || !mBootstrapTransport || !mShutdownTrigger) return INVALID_OPERATION;
        if (mShutdownTrigger->isTriggered() || mConnections.mIncoming.size() < mConnections.mMaxIncoming)
            return DEAD_OBJECT;
        previous = mMaxIncomingThreads;
        if (threads <= previous) return OK;
        mMaxIncomingThreads = threads;
    }
    // Reuse the original connection handshake, worker, dispatch and shutdown
    // ownership. No client callback runs under the session's state mutex.
    for (size_t i = previous; i < threads; ++i) {
        status_t status = setupBootstrapConnection(mId, true);
        if (status != OK) {
            RpcMutexLockGuard lock(mMutex);
            mMaxIncomingThreads = mConnections.mIncoming.size();
            return status;
        }
    }
    return OK;
}`+source.slice(end+'\n    });\n}'.length);
  // Connection setup failure must reach the caller instead of claiming a live
  // worker after the original readConnectionInit has already failed.
  source=replace(source,'    bool ownershipTransferred = false;',
    '    bool ownershipTransferred = false;\n    status_t connectionStatus = UNKNOWN_ERROR;');
  source=replace(source,'        ownershipTransferred = true;',
    '        connectionStatus = setupResult.status;\n        ownershipTransferred = true;');
  source=replace(source,'    LOG_ALWAYS_FATAL_IF(!ownershipTransferred);\n    return OK;',
    '    LOG_ALWAYS_FATAL_IF(!ownershipTransferred);\n    return connectionStatus;');
  return source;
}

export function adaptProcessPoolSource(file,source){
  if(file.endsWith('BackendUnifiedServiceManager.h'))return replace(source,
    '    explicit BackendUnifiedServiceManager(const sp<os::IServiceManager>& impl);',
    '    explicit BackendUnifiedServiceManager(const sp<os::IServiceManager>& impl);\n    const sp<os::IServiceManager>& hostImplementation() const { return mTheRealServiceManager; }');
  if(file.endsWith('ProcessState.h'))return replace(source,
    '    LIBBINDER_EXPORTED void startThreadPool();',
    `    LIBBINDER_EXPORTED void startThreadPool();
    size_t hostRpcThreadPoolLimit() const { return mMaxThreads.load() + 1; }
    void hostRpcThreadPoolStarted() { mThreadPoolStarted = true; }`);
  if(!file.endsWith('ProcessState.cpp'))throw Error('Unexpected original process pool source: '+file);
  source=body(source,'void ProcessState::startThreadPool()',`    auto manager = getBackendUnifiedServiceManager();
    auto binder = IInterface::asBinder(manager->hostImplementation());
    auto proxy = binder ? binder->remoteBinder() : nullptr;
    LOG_ALWAYS_FATAL_IF(!proxy || !proxy->isRpcBinder(), "Native service directory is not an RPC endpoint");
    status_t status = proxy->hostRpcSession()->growIncomingThreadPool(hostRpcThreadPoolLimit());
    LOG_ALWAYS_FATAL_IF(status != OK, "Native Binder thread pool could not start: %d", status);
    mThreadPoolStarted = true;`);
  source=body(source,'void ProcessState::spawnPooledThread(bool isMain)',`    (void)isMain;
    if (mThreadPoolStarted) startThreadPool();`);
  source=body(source,'status_t ProcessState::setThreadPoolMaxThreadCount(size_t maxThreads)',`    LOG_ALWAYS_FATAL_IF(mThreadPoolStarted && maxThreads < mMaxThreads,
                        "Binder threadpool cannot be shrunk after starting");
    if (maxThreads == SIZE_MAX) return BAD_VALUE;
    if (!mThreadPoolStarted) { mMaxThreads = maxThreads; return OK; }
    auto manager = getBackendUnifiedServiceManager();
    auto binder = IInterface::asBinder(manager->hostImplementation());
    auto proxy = binder ? binder->remoteBinder() : nullptr;
    if (!proxy || !proxy->isRpcBinder()) return NO_INIT;
    status_t status = proxy->hostRpcSession()->growIncomingThreadPool(maxThreads + 1);
    if (status == OK) mMaxThreads = maxThreads;
    return status;`);
  source=body(source,'size_t ProcessState::getThreadPoolMaxTotalThreadCount() const',`    if (!mThreadPoolStarted) return 0;
    auto manager = getBackendUnifiedServiceManager();
    auto binder = IInterface::asBinder(manager->hostImplementation());
    auto proxy = binder ? binder->remoteBinder() : nullptr;
    return proxy && proxy->isRpcBinder() ? proxy->hostRpcSession()->getMaxIncomingThreads() : 0;`);
  return includeDependencies(source,'#include "BackendUnifiedServiceManager.h"\n#include <binder/RpcSession.h>');
}
