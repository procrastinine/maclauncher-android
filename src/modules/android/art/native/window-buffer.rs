use super::*;

impl Window {
    /// Own the buffer used by the next frame before exposing its framebuffer.
    /// A window's query dimensions are consumer defaults; dequeue supplies the
    /// producer's requested extent, including explicit buffer geometry.
    pub(super) fn acquire_buffer(&mut self, surface: Option<EGLSurface>) -> bool {
        if !self.buffer.is_null() {
            return true;
        }
        let (mut buffer, mut fence) = (std::ptr::null_mut(), -1);
        // SAFETY: this surface retains its native window.
        if unsafe { ANativeWindow_dequeueBuffer(self.window, &mut buffer, &mut fence) } != 0 {
            return fail(EGL_BAD_NATIVE_WINDOW, false);
        }
        if fence >= 0 {
            // A current surface can queue the original GPU-side dependency.
            // Initial creation has no current context, so wait before exposing it.
            let ready = if surface.is_some() {
                crate::sync::server_wait(self.display,fence)==EGL_TRUE
            } else {
                match crate::sync::poll(fence,u64::MAX) {
                    Ok(true)=>true,
                    Ok(false)=>fail(EGL_BAD_ALLOC,false),
                    Err(error)=>fail(error,false),
                }
            };
            if !ready {unsafe{ANativeWindow_cancelBuffer(self.window,buffer,fence)};return false;}
            unsafe{libc::close(fence)};
        }
        if buffer.is_null() {
            return fail(EGL_BAD_NATIVE_WINDOW, false);
        }
        self.buffer = buffer;
        // SAFETY: the native window owns the dequeued buffer until queue/cancel.
        let (width, height) = unsafe { ((*buffer).width, (*buffer).height) };
        if width <= 0 || height <= 0 {
            self.cancel_buffer();
            return fail(EGL_BAD_NATIVE_WINDOW, false);
        }
        if (width, height) == (self.width, self.height) && !self.pbuffer.is_null() {
            return true;
        }
        let pbuffer = self.create_pbuffer(width, height);
        if pbuffer.is_null() {
            self.cancel_buffer();
            return false;
        }
        if let Some(surface) = surface {
            let (draw, read) = CURRENT.get();
            if draw == surface as usize {
                let read = if read == draw { pbuffer } else { host_surface(read as EGLSurface) };
                // SAFETY: preserve this thread's context and independent read surface.
                let okay = unsafe {
                    host::eglMakeCurrent(self.display, pbuffer, read, host::eglGetCurrentContext())
                };
                if okay != EGL_TRUE {
                    unsafe { host::eglDestroySurface(self.display, pbuffer) };
                    self.cancel_buffer();
                    return false;
                }
            }
        }
        if !self.pbuffer.is_null() {
            unsafe { host::eglDestroySurface(self.display, self.pbuffer) };
        }
        self.pbuffer = pbuffer;
        self.width = width;
        self.height = height;
        self.release_images();
        true
    }

    pub(super) fn cancel_buffer(&mut self) {
        let buffer = std::mem::replace(&mut self.buffer, std::ptr::null_mut());
        if !buffer.is_null() {
            // SAFETY: our acquire fence has been waited and closed. No copy was queued.
            unsafe { ANativeWindow_cancelBuffer(self.window, buffer, -1) };
        }
    }
}
