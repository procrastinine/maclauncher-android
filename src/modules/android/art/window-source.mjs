import fs from 'node:fs';
import path from 'node:path';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';

/** Match EGL's framebuffer to the original native queue's producer buffer. */
export function prepareWindowBuffers(aim) {
 const file=path.join(aim,'hal/gles/src/egl.rs');let text=fs.readFileSync(file,'utf8');
 const replace=(before,after)=>{if(text.split(before).length!==2)throw Error('ART EGL window boundary changed: '+before.slice(0,100));text=text.replace(before,after);};
 replace('    fn ANativeWindow_getWidth(w: *mut NativeWindow) -> i32;\n    fn ANativeWindow_getHeight(w: *mut NativeWindow) -> i32;\n','');
 replace('fn ANativeWindow_setBuffersGeometry(w: *mut NativeWindow, width: c_int, height: c_int, format: c_int) -> c_int;',
         'fn ANativeWindow_setBuffersFormat(w: *mut NativeWindow, format: c_int) -> c_int;');
 replace('struct Window {','#[path = "window-buffer.rs"]\nmod window_buffer;\n\nstruct Window {');
 replace('    window: *mut NativeWindow,\n    /// The host pbuffer', '    window: *mut NativeWindow,\n    /// The dequeued native buffer owned by the next frame.\n    buffer: *mut NativeBuffer,\n    /// The host pbuffer');
 const start=text.indexOf('    // SAFETY: a valid window.\n    let (width, height) = unsafe {',text.indexOf('unsafe fn create_window('));
 const end=text.indexOf('    if configs::owner(config).is_some() {',start);
 if(start<0||end<0)throw Error('ART EGL window creation changed');text=text.slice(0,start)+text.slice(end);
 replace('ANativeWindow_setBuffersGeometry(native, 0, 0, 2)', 'ANativeWindow_setBuffersFormat(native, 2)');
 replace('        window: native,\n        pbuffer: std::ptr::null_mut(),\n        width,\n        height,', '        window: native,\n        buffer: std::ptr::null_mut(),\n        pbuffer: std::ptr::null_mut(),\n        width: 0,\n        height: 0,');
 replace(`    w.pbuffer = w.create_pbuffer(width, height);
    if w.pbuffer.is_null() {
        // The host's error stands.
        return std::ptr::null_mut();
    }
    // SAFETY: a valid window; released in eglDestroySurface.
    unsafe { ANativeWindow_acquire(native) };`, `    // SAFETY: retained through the owned buffer and released on every failure.
    unsafe { ANativeWindow_acquire(native) };
    if !w.acquire_buffer(None) {
        unsafe { ANativeWindow_release(native) };
        return std::ptr::null_mut();
    }`);
 replace('    w.release_images();\n    // SAFETY: the surface\'s pbuffer and window reference.', '    w.cancel_buffer();\n    w.release_images();\n    // SAFETY: the surface\'s pbuffer and window reference.');
 replace(`    let (mut buffer, mut fence) = (std::ptr::null_mut(), -1);
    // SAFETY: the surface's window.
    if unsafe { ANativeWindow_dequeueBuffer(w.window, &mut buffer, &mut fence) } != 0 {
        return fail(EGL_BAD_NATIVE_WINDOW, EGL_FALSE);
    }
    if fence >= 0 {
        let waited = crate::sync::server_wait(w.display, fence);
        // SAFETY: the host wait has retained the dependency on success;
        // on failure the untouched buffer goes back with its fence.
        unsafe {
            if waited == EGL_FALSE {
                ANativeWindow_cancelBuffer(w.window, buffer, fence);
                return EGL_FALSE;
            }
            libc::close(fence);
        }
    }`, `    if !w.acquire_buffer(Some(surface)) {
        return EGL_FALSE;
    }
    let buffer = std::mem::replace(&mut w.buffer, std::ptr::null_mut());`);
 const swapStart=text.indexOf('    let presented = aim_hostcall::guest::gpu_present(&mut present).is_ok();');
 const swapEnd=text.indexOf('\n    EGL_TRUE\n}',swapStart);
 if(swapStart<0||swapEnd<0)throw Error('ART EGL window swap changed');
 text=text.slice(0,swapStart)+`    if aim_hostcall::guest::gpu_present(&mut present).is_err() {
        // The copy did not complete; return the buffer with any issued fence.
        unsafe { ANativeWindow_cancelBuffer(w.window, buffer, present.fence) };
        return fail(EGL_BAD_SURFACE, EGL_FALSE);
    }
    // The queue consumes the copy fence even if queueing reports an error.
    if unsafe { ANativeWindow_queueBuffer(w.window, buffer, present.fence) } != 0 {
        unsafe { ANativeWindow_cancelBuffer(w.window, buffer, -1) };
        return fail(EGL_BAD_SURFACE, EGL_FALSE);
    }
    // Acquire the following frame now, so its actual geometry is current before drawing.
    if !w.acquire_buffer(Some(surface)) {
        return EGL_FALSE;
    }`+text.slice(swapEnd);
 replace('//!   framebuffer. `eglSwapBuffers` dequeues a buffer, has the host blit the\n//!   pbuffer into it (top row first) and queues it with the blit\'s fence.',
         '//!   framebuffer, sized to its owned dequeued buffer. `eglSwapBuffers` blits\n//!   into that buffer, queues it with the copy fence and acquires the next frame.');
 copyChanged(path.join(import.meta.dirname,'native/window-buffer.rs'),path.join(aim,'hal/gles/src/window-buffer.rs'));
 writeChanged(file,text);
}
