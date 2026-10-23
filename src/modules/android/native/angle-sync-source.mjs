// EGL client waits belong to a display, and may run without a current context.
// Keep the upstream Metal queue/event implementation; supply its actual display
// independently of the optional context used for EGL_SYNC_FLUSH_COMMANDS_BIT.
export function adaptAngleMetalSync(source) {
  const replace = (from, to, count = 1) => {
    if (source.split(from).length !== count + 1) throw Error('ANGLE Metal synchronization source changed');
    source = source.replaceAll(from, to);
  };
  replace('clientWait(ContextMtl *contextMtl,', 'clientWait(DisplayMtl *displayMtl,\n                             ContextMtl *contextMtl,', 3);
  replace('if (flushCommands)', 'if (flushCommands && contextMtl)', 2);
  replace('contextMtl->getDisplay()->getOrCreateSharedEventListener()', 'displayMtl->getOrCreateSharedEventListener()');
  // The listener is lazy display state. Acquire it while set() still owns the
  // EGL lock and retain it with the event, never initialize it in the tail call.
  replace('        mSignalValue = signalValue;',
    '        mSignalValue = signalValue;\n        mEventListener = contextMtl->getDisplay()->getOrCreateSharedEventListener();');
  replace('            displayMtl->getOrCreateSharedEventListener();', '            mEventListener;');
  replace('    angle::ObjCPtr<id<MTLSharedEvent>> mMetalSharedEvent;',
    '    angle::ObjCPtr<id<MTLSharedEvent>> mMetalSharedEvent;\n    angle::ObjCPtr<MTLSharedEventListener> mEventListener;');
  replace('DisplayMtl *display = contextMtl->getDisplay();', 'DisplayMtl *display = displayMtl;');
  replace('mSync->clientWait(contextMtl, true,', 'mSync->clientWait(contextMtl->getDisplay(), contextMtl, true,');
  replace('mSync->clientWait(contextMtl, flush, timeout, outResult)', 'mSync->clientWait(contextMtl->getDisplay(), contextMtl, flush, timeout, outResult)');
  replace('ContextMtl *contextMtl = mtl::GetImpl(context);\n    if (IsError(mSync->clientWait(contextMtl, flush, static_cast<uint64_t>(timeout), &result)))',
    'ContextMtl *contextMtl = context ? mtl::GetImpl(context) : nullptr;\n    if (IsError(mSync->clientWait(mtl::GetImpl(display), contextMtl, flush, static_cast<uint64_t>(timeout), &result)))');
  // Follow ANGLE's Vulkan/GL EGL backends: only context preparation runs while
  // the entrypoint owns the global EGL sync lock. Waiting under that lock can
  // prevent another thread from submitting the commands that signal the fence.
  replace('    if (IsError(mSync->clientWait(mtl::GetImpl(display), contextMtl, flush, static_cast<uint64_t>(timeout), &result)))',
    '    if (IsError(mSync->clientWait(mtl::GetImpl(display), contextMtl, flush, 0, &result)))');
  // EGL fences are cross-context synchronization points. Match ANGLE Vulkan's
  // CurrentContextToAllContexts submission: a worker must not depend on the
  // producer reaching a later swap before the fence can make forward progress.
  // In particular, frame pacers create their fence before waiting for vsync.
  replace('            mSync = std::move(impl);\n\n            break;',
    '            mSync = std::move(impl);\n            contextMtl->flushCommandBuffer(mtl::NoWait);\n\n            break;');
  replace('    switch (result)\n    {\n        case GL_ALREADY_SIGNALED:', `    if (result == GL_TIMEOUT_EXPIRED && timeout != 0)
    {
        // Retain the implementation: eglDestroySync may run while this wait
        // has released the global lock. No calling context is used unlocked.
        egl::Display::GetCurrentThreadUnlockedTailCall()->add(
            [sync = mSync, displayMtl = mtl::GetImpl(display), timeout](void *resultOut) {
                GLenum waited = GL_NONE;
                const bool failed = IsError(sync->clientWait(
                    displayMtl, nullptr, false, static_cast<uint64_t>(timeout), &waited));
                if (resultOut)
                {
                    *static_cast<EGLint *>(resultOut) = failed ? EGL_FALSE
                        : waited == GL_TIMEOUT_EXPIRED ? EGL_TIMEOUT_EXPIRED_KHR
                        : EGL_CONDITION_SATISFIED_KHR;
                }
            });
        return egl::NoError();
    }

    switch (result)
    {
        case GL_ALREADY_SIGNALED:`);
  return adaptAngleMetalSyncAllocation(source);
}

export function adaptAngleMetalSyncHeader(source) {
  const before = '    angle::ObjCPtr<id<MTLSharedEvent>> mSharedEvent;\n\n    std::unique_ptr<mtl::SyncImpl> mSync;';
  if (source.split(before).length !== 2) throw Error('ANGLE Metal synchronization header changed');
  return source.replace(before, before.replace('std::unique_ptr', 'std::shared_ptr'));
}


// A sync must refer to a real command buffer. Objective-C messages to nil do
// nothing; a queue serial alone cannot establish that its event will signal.
export function adaptAngleMetalSyncAllocation(source) {
  const replace = (before, after) => {
    if (source.split(before).length !== 2) throw Error('ANGLE Metal sync allocation source changed');
    source = source.replace(before, after);
  };
  replace('        mMetalSharedEvent = std::move(sharedEvent);',
    '        if (!sharedEvent)\n        {\n            return angle::Result::Stop;\n        }\n        mMetalSharedEvent = std::move(sharedEvent);');
  replace('            contextMtl->queueEventSignal(mMetalSharedEvent, mSignalValue);',
    '            if (contextMtl->queueEventSignal(mMetalSharedEvent, mSignalValue) == 0)\n            {\n                return angle::Result::Stop;\n            }');
  replace('        mEncodedCommandBufferSerial = contextMtl->queueEventSignal(mMetalEvent, kEventSignalValue);\n        return angle::Result::Continue;',
    '        if (!mMetalEvent)\n        {\n            return angle::Result::Stop;\n        }\n        mEncodedCommandBufferSerial = contextMtl->queueEventSignal(mMetalEvent, kEventSignalValue);\n        return mEncodedCommandBufferSerial == 0 ? angle::Result::Stop : angle::Result::Continue;');
  return source;
}

export function adaptAngleMetalSyncContext(source) {
  const before = 'uint64_t ContextMtl::queueEventSignal(id<MTLEvent> event, uint64_t value)\n{\n    ensureCommandBufferReady();';
  if (source.split(before).length !== 2) throw Error('ANGLE Metal sync context source changed');
  return source.replace(before, `uint64_t ContextMtl::queueEventSignal(id<MTLEvent> event, uint64_t value)
{
    if (!event)
    {
        return 0;
    }
    // Unlike ordinary draw preparation, sync creation can report allocation
    // failure before exporting a fence. Do not treat an unbacked serial as work.
    flushCommandBufferIfNeeded();
    if (!mCmdBuffer.ready())
    {
        mCmdBuffer.restart();
    }
    if (!mCmdBuffer.ready())
    {
        return 0;
    }`);
}

export function adaptAngleMetalCommandBuffers(source) {
  const replace = (before, after) => {
    if (source.split(before).length !== 2) throw Error('ANGLE Metal command buffer allocation source changed');
    source = source.replace(before, after);
  };
  replace('        angle::ObjCPtr<id<MTLCommandBuffer>> metalCmdBuffer = [get() commandBuffer];\n\n        std::lock_guard<std::mutex> lg(mLock);',
    `        angle::ObjCPtr<id<MTLCommandBuffer>> metalCmdBuffer = [get() commandBuffer];
        if (!metalCmdBuffer)
        {
            // No command buffer exists to complete this serial or its events.
            mCmdBufferError.store(MTLCommandBufferErrorOutOfMemory);
            *queueSerialOut = 0;
            return metalCmdBuffer;
        }

        std::lock_guard<std::mutex> lg(mLock);`);
  replace('    mLastWaitOp  = mtl::NoWait;\n\n    for (std::string &marker : mDebugGroups)',
    '    mLastWaitOp  = mtl::NoWait;\n    if (!metalCmdBuffer)\n    {\n        clearResourceListAndSize();\n        return;\n    }\n\n    for (std::string &marker : mDebugGroups)');
  replace('    ASSERT(readyImpl());\n\n    CommandEncoder *currentEncoder = getPendingCommandEncoder();',
    '    if (!readyImpl() || !event)\n    {\n        return 0;\n    }\n\n    maclauncher::observeNativeFenceCompletion(get(), event, value);\n    CommandEncoder *currentEncoder = getPendingCommandEncoder();');
  replace('#include "libANGLE/renderer/metal/mtl_command_buffer.h"',
    '#include "libANGLE/renderer/metal/mtl_command_buffer.h"\n#include "angle-fence-completion.h"');
  return source;
}
