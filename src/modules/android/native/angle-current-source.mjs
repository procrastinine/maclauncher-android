// Preserve the original EGL binding across failed context initialization.
export function adaptAngleCurrentBindings(source) {
  const before = `    bool contextChanged = context != previousContext;
    if (previousContext != nullptr && contextChanged)
    {
        // Need AddRefLock because there may be ContextMutex destruction.
        ScopedContextMutexAddRefLock lock(previousContext->getContextMutex());

        previousContext->release();
        thread->setCurrent(nullptr);

        auto error = previousContext->unMakeCurrent(this);
        if (!previousContext->isReferenced() && previousContext->isDestroyed())
        {
            // The previous Context may have been created with a different Display.
            Display *previousDisplay = previousContext->getDisplay();
            ANGLE_TRY(previousDisplay->releaseContext(previousContext, thread));
        }
        ANGLE_TRY(error);
    }

    {
        ScopedContextMutexLock lock(context != nullptr ? &context->getContextMutex() : nullptr);

        thread->setCurrent(context);

        ANGLE_TRY(mImplementation->makeCurrent(this, drawSurface, readSurface, context));

        if (context != nullptr)
        {
            ANGLE_TRY(context->makeCurrent(this, drawSurface, readSurface));
            if (contextChanged)
            {
                context->addRef();
            }
        }
    }
`;
  const after = `    bool contextChanged = context != previousContext;
    Surface *previousDraw = previousContext ? previousContext->getCurrentDrawSurface() : nullptr;
    Surface *previousRead = previousContext ? previousContext->getCurrentReadSurface() : nullptr;
    Display *previousDisplay = previousContext ? previousContext->getDisplay() : this;
    ScopedSurfaceRef keepDraw(previousDraw);
    ScopedSurfaceRef keepRead(previousRead);
    bool attemptedContext = false;

    // Keep the outgoing binding's original reference until the replacement
    // commits. A failed makeCurrent can unset its framebuffer before returning.
    // Publishing that partially initialized context in TLS would make every
    // later GL entry observe an invalid current context.
    auto restorePrevious = [&]() {
        if (context && attemptedContext)
        {
            ScopedContextMutexLock lock(&context->getContextMutex());
            (void)context->unMakeCurrent(this);
        }
        thread->setCurrent(previousContext);
        if (previousContext)
        {
            ScopedContextMutexLock lock(&previousContext->getContextMutex());
            auto restored = previousDisplay->mImplementation->makeCurrent(
                previousDisplay, previousDraw, previousRead, previousContext);
            if (!restored.isError())
            {
                restored = previousContext->makeCurrent(previousDisplay, previousDraw, previousRead);
            }
            if (restored.isError())
            {
                // A genuine device failure can prevent restoration as well.
                // Retain its binding ownership but prevent GL from using an
                // incomplete framebuffer; preserve the original EGL error.
                previousContext->markContextLost(gl::GraphicsResetStatus::UnknownContextReset);
            }
        }
        else
        {
            (void)mImplementation->makeCurrent(this, nullptr, nullptr, nullptr);
        }
    };

    if (previousContext != nullptr && contextChanged)
    {
        egl::Error error = egl::NoError();
        {
            ScopedContextMutexLock lock(&previousContext->getContextMutex());
            thread->setCurrent(nullptr);
            error = previousContext->unMakeCurrent(previousDisplay);
        }
        if (error.isError())
        {
            restorePrevious();
            return error;
        }
    }

    egl::Error error = egl::NoError();
    {
        ScopedContextMutexLock lock(context != nullptr ? &context->getContextMutex() : nullptr);
        thread->setCurrent(context);
        error = mImplementation->makeCurrent(this, drawSurface, readSurface, context);
        if (!error.isError() && context != nullptr)
        {
            attemptedContext = true;
            error = context->makeCurrent(this, drawSurface, readSurface);
            if (!error.isError() && contextChanged)
            {
                context->addRef();
            }
        }
    }
    if (error.isError())
    {
        restorePrevious();
        return error;
    }
    if (previousContext != nullptr && contextChanged)
    {
        ScopedContextMutexAddRefLock lock(previousContext->getContextMutex());
        previousContext->release();
        if (!previousContext->isReferenced() && previousContext->isDestroyed())
        {
            ANGLE_TRY(previousDisplay->releaseContext(previousContext, thread));
        }
    }
`;
  if (source.split(before).length !== 2) throw Error('ANGLE current-context source changed');
  return source.replace(before, after);
}
