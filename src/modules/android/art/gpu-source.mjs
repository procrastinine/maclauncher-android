import fs from 'node:fs';
import path from 'node:path';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';
import {verifyGlesHeaders} from '../native/gles-headers.mjs';

/** The caller may supply its existing pinned checkout; standalone builds use
 * the registered nested source by default. Verify before publishing headers. */
export function prepareGlesInteropHeaders({glesHeaders,output}) {
  const pin=JSON.parse(fs.readFileSync(new URL('../native/gles-headers.json',import.meta.url)));
  const root=fs.realpathSync(glesHeaders??path.resolve(import.meta.dirname,'../../../..',pin.directory));
  const manifest=verifyGlesHeaders(root);
  for(const file of manifest.files.filter(file=>/^include\/(?:EGL|KHR)\//.test(file.path)))
    copyChanged(path.join(root,file.path),path.join(output,file.path));
  return {root,manifest};
}

export function prepareAimGlesInterop(aim,{glesHeaders}={}) {
  const patch=(file,changes)=>{
    const target=path.join(aim,file);let text=fs.readFileSync(target,'utf8');
    for(const [before,after] of changes){
      if(text.split(before).length!==2)throw Error('ART GLES boundary changed: '+file);
      text=text.replace(before,after);
    }
    writeChanged(target,text);
  };
  const native=path.join(import.meta.dirname,'native');
  const cacheRoot=path.join(aim,'crates/aim-host-gpu/src/mac-video');
  copyChanged(path.join(native,'egl-cache.mm'),path.join(cacheRoot,'egl-cache.mm'));
  copyChanged(path.join(native,'../..','native/native-egl-cache.hpp'),path.join(cacheRoot,'native-egl-cache.hpp'));
  prepareGlesInteropHeaders({glesHeaders,output:cacheRoot});
  copyChanged(path.join(native,'egl-cache.rs'),path.join(aim,'crates/aim-host-gpu/src/egl-cache.rs'));
  copyChanged(path.join(native,'buffer-alias.rs'),path.join(aim,'crates/aim-hostcall/src/buffer-alias.rs'));
  copyChanged(path.join(native,'gles-vulkan.rs'),path.join(aim,'crates/aim-host-gpu/src/vulkan.rs'));
  copyChanged(path.join(native,'present-resolve.rs'),path.join(aim,'crates/aim-host-gpu/src/present-resolve.rs'));
  patch('crates/aim-host-gpu/src/present.rs',[
    ['struct Gl {', '#[derive(Clone, Copy)]\nstruct Gl {'],
    ['    blit_angle: Blit,', '    blit_angle: Blit,\n    blit_nv: Blit,'],
    ['                blit_angle: entry(c"glBlitFramebufferANGLE")?,', '                blit_angle: entry(c"glBlitFramebufferANGLE")?,\n                blit_nv: entry(c"glBlitFramebufferNV")?,'],
    ['use aim_hostcall::gpu::Present;', 'use aim_hostcall::gpu::Present;\n#[path = "present-resolve.rs"]\nmod present_resolve;'],
    ['    get_string: unsafe extern "C" fn(u32) -> *const i8,', '    get_string: unsafe extern "C" fn(u32) -> *const i8,\n    gen_renderbuffers: unsafe extern "C" fn(i32, *mut u32),\n    delete_renderbuffers: unsafe extern "C" fn(i32, *const u32),\n    bind_renderbuffer: unsafe extern "C" fn(u32, u32),\n    renderbuffer_storage: unsafe extern "C" fn(u32, u32, i32, i32),\n    framebuffer_renderbuffer: unsafe extern "C" fn(u32, u32, u32, u32),\n    framebuffer_attachment_parameter: unsafe extern "C" fn(u32, u32, u32, *mut i32),'],
    ['                get_string: entry(c"glGetString")?,', '                get_string: entry(c"glGetString")?,\n                gen_renderbuffers: entry(c"glGenRenderbuffers")?,\n                delete_renderbuffers: entry(c"glDeleteRenderbuffers")?,\n                bind_renderbuffer: entry(c"glBindRenderbuffer")?,\n                renderbuffer_storage: entry(c"glRenderbufferStorage")?,\n                framebuffer_renderbuffer: entry(c"glFramebufferRenderbuffer")?,\n                framebuffer_attachment_parameter: entry(c"glGetFramebufferAttachmentParameteriv")?,'],
    ['    flip_y: bool,', '    flip_y: bool,\n    nv_blit: bool,\n    resolve: Option<present_resolve::Resolve>,'],
    ['unsafe fn target(g: &Gl, display: usize, context: usize, image: usize) -> (u32, i32, bool) {', 'unsafe fn target(g: &Gl, display: usize, context: usize, image: usize, extent: (i32, i32), multisampled: bool, legacy_format: Option<u32>) -> (u32, i32, bool, u32, bool) {'],
    ['            (g.delete_textures)(1, &x.texture);', '            (g.delete_textures)(1, &x.texture);\n            if let Some(resolve) = x.resolve { resolve.retire(g); }'],
    ['        .iter()\n        .find(|x| x.context == context && x.image == image)', '        .iter_mut()\n        .find(|x| x.context == context && x.image == image)'],
    ['        return (x.fbo, x.version, x.flip_y);', '        let resolved = if multisampled && (x.version >= 3 || x.nv_blit) { unsafe { present_resolve::buffer(g, &mut x.resolve, extent.0, extent.1, legacy_format) } } else { 0 };\n        return (x.fbo, x.version, x.flip_y, resolved, x.nv_blit);'],
    ['    // SAFETY: caller contract; the texture binding is restored.', '    let nv_blit = !extensions.is_null() && unsafe { CStr::from_ptr(extensions) }\n        .to_bytes().split(|&c| c == b\' \').any(|x| x == b"GL_NV_framebuffer_blit");\n    // SAFETY: caller contract; the texture binding is restored.'],
    ['    live.push(Target {', '    let mut resolve = None;\n    let resolved = if multisampled && (version >= 3 || nv_blit) { unsafe { present_resolve::buffer(g, &mut resolve, extent.0, extent.1, legacy_format) } } else { 0 };\n    live.push(Target {'],
    ['        flip_y,\n    });\n    (fbo, version, flip_y)', '        flip_y,\n        nv_blit,\n        resolve,\n    });\n    (fbo, version, flip_y, resolved, nv_blit)'],
    ['        let (fbo, version, flip_y) = target(g, display, context, p.image as usize);', '        // Query the actual default framebuffer, not an application FBO that\n        // happens to remain bound at eglSwapBuffers. Restore both below.\n        (g.bind_framebuffer)(GL_DRAW_FRAMEBUFFER, 0);\n        (g.bind_framebuffer)(GL_READ_FRAMEBUFFER, 0);\n        let mut samples = 0;\n        (g.get_integerv)(0x80a9, &mut samples);\n        let legacy_format = if version < 3 && samples > 0 { Some(present_resolve::legacy_format(g)) } else { None };\n        let (fbo, version, flip_y, resolved, nv_blit) = target(g, display, context, p.image as usize, (p.src_width as i32, p.src_height as i32), samples > 0, legacy_format);'],
    ['        if version >= 3 {\n            let filter', '        if resolved != 0 {\n            // Resolve with identical bounds before any flip or scaling.\n            (g.bind_framebuffer)(GL_DRAW_FRAMEBUFFER, resolved);\n            let blit = if version >= 3 { g.blit } else { g.blit_nv };\n            (blit)(0, 0, sw, sh, 0, 0, sw, sh, GL_COLOR_BUFFER_BIT, GL_NEAREST);\n            (g.bind_framebuffer)(GL_READ_FRAMEBUFFER, resolved);\n            (g.bind_framebuffer)(GL_DRAW_FRAMEBUFFER, fbo);\n        }\n        if version >= 3 {\n            let filter'],
    ['        } else {\n            // ANGLE_framebuffer_blit copies without scaling.', '        } else if nv_blit {\n            // NV permits the same orientation/scaling bounds as core ES3.\n            // ANGLE_framebuffer_blit explicitly forbids reversed rectangles.\n            let filter = if (sw, sh) == (dw, dh) { GL_NEAREST } else { GL_LINEAR };\n            let (y0, y1) = if flip_y { (0, dh) } else { (dh, 0) };\n            (g.blit_nv)(0, 0, sw, sh, 0, y0, dw, y1, GL_COLOR_BUFFER_BIT, filter);\n        } else {\n            // ANGLE_framebuffer_blit copies without scaling.'],
    ['/// A context\'s framebuffer over a buffer\'s image.', `// GLES1 exposes framebuffer objects through OES, not the ES2 core entry
// points. ANGLE exports both but correctly rejects the core API on ES1.
fn es1_gl(core: &Gl) -> Option<&'static Gl> {
    static GL: OnceLock<Option<Gl>> = OnceLock::new();
    GL.get_or_init(|| unsafe {
        let mut gl = *core;
        gl.gen_framebuffers = entry(c"glGenFramebuffersOES")?;
        gl.delete_framebuffers = entry(c"glDeleteFramebuffersOES")?;
        gl.bind_framebuffer = entry(c"glBindFramebufferOES")?;
        gl.framebuffer_texture = entry(c"glFramebufferTexture2DOES")?;
        gl.gen_renderbuffers = entry(c"glGenRenderbuffersOES")?;
        gl.delete_renderbuffers = entry(c"glDeleteRenderbuffersOES")?;
        gl.bind_renderbuffer = entry(c"glBindRenderbufferOES")?;
        gl.renderbuffer_storage = entry(c"glRenderbufferStorageOES")?;
        gl.framebuffer_renderbuffer = entry(c"glFramebufferRenderbufferOES")?;
        gl.framebuffer_attachment_parameter = entry(c"glGetFramebufferAttachmentParameterivOES")?;
        Some(gl)
    }).as_ref()
}

/// A context's framebuffer over a buffer's image.`],
    ['        let display = (g.get_current_display)();', `        let display = (g.get_current_display)();
        let mut version = 0;
        (g.query_context)(display, context, EGL_CONTEXT_CLIENT_VERSION, &mut version);
        let g = if version < 2 { let Some(legacy) = es1_gl(g) else { return EINVAL; }; legacy } else { g };`],
  ]);
  for(const crate of ['aim-host-gpu','aim-host-vulkan']){
    patch('crates/'+crate+'/src/metal.rs',[
      ['use std::ffi::{CStr, c_char, c_void};', 'use std::ffi::{CStr, c_char, c_void};\n#[path = "../../aim-hostcall/src/buffer-alias.rs"]\nmod alias;'],
      ['        let buffer = send!(device, c"newBufferWithBytesNoCopy:length:options:deallocator:" => Id,\n            *mut c_void = a.address as *mut c_void, usize = a.length as usize,\n            usize = MTL_STORAGE_MODE_SHARED, Id = std::ptr::null_mut());',
        '        let alias = alias::Mapping::new(a.address, a.length)?;\n        let deallocator = alias.block();\n        let buffer = send!(device, c"newBufferWithBytesNoCopy:length:options:deallocator:" => Id,\n            *mut c_void = alias.address as *mut c_void, usize = a.length as usize,\n            usize = MTL_STORAGE_MODE_SHARED, *const alias::Block = &deallocator);'],
      ['        // SAFETY: a class name.', '        alias.transferred();\n        // SAFETY: a class name.'],
    ]);
  }
  patch('crates/aim-host-vulkan/src/metal.rs',[
    ['use crate::{EBADF, EINVAL, Private};', 'use super::{EBADF, EINVAL, Private};'],
  ]);
  patch('crates/aim-host-gpu/src/lib.rs',[
    ['mod display;', 'mod display;\nmod vulkan;\n#[path = "egl-cache.rs"]\nmod egl_cache;'],
    ['        for (e, slot) in table::ENTRIES.iter().zip(&RESOLVED) {', '        egl_cache::provider(egl);\n        for (e, slot) in table::ENTRIES.iter().zip(&RESOLVED) {'],
    ['                display::initialized(handle);', '                if display::initialized(handle) { egl_cache::initialized(regs[0] as usize); }'],
    ['            } else if r as u32 == 1\n                && (e.name == c"eglDestroyImageKHR"', '            } else if e.name == c"eglTerminate" && r as u32 == 1 {\n                display::terminated(handle);\n            } else if r as u32 == 1\n                && (e.name == c"eglDestroyImageKHR"'],
    ['Ok(a) => metal::import_buffer(a),', 'Ok(a) => if vulkan::selected() { vulkan::import_buffer(a) } else { metal::import_buffer(a) },'],
  ]);
  patch('crates/aim-host-gpu/src/display.rs',[
    ['    pub fn initialized(&mut self, handle: usize) {\n        if let Some(s) = self.slot(handle) {\n            s.initialized = true;\n        }\n    }', `    pub fn initialized(&mut self, handle: usize) -> bool {
        let Some(index) = handle.checked_sub(HANDLE_BASE).filter(|&index| index < self.slots.len()) else { return false; };
        let display = self.slots[index].display;
        let first = display != 0 && !self.slots.iter().any(|slot| slot.display == display && slot.initialized);
        self.slots[index].initialized = true;
        first
    }

    pub fn terminated(&mut self, handle: usize) {
        let Some(display) = self.slot(handle).map(|slot| slot.display) else { return; };
        for slot in &mut self.slots { if slot.display == display { slot.initialized = false; } }
    }`],
    ['            init(display, std::ptr::null_mut(), std::ptr::null_mut());', '            if init(display, std::ptr::null_mut(), std::ptr::null_mut()) == 1 { crate::egl_cache::initialized(display); }'],
    ['pub fn initialized(display: usize) {\n    displays().initialized(display);\n}', 'pub fn initialized(display: usize) -> bool {\n    displays().initialized(display)\n}\npub fn terminated(display: usize) {\n    displays().terminated(display);\n}'],
  ]);
  patch('hal/gles/src/egl.rs',[
    ['        EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,', '        if std::env::var("KL_ANGLE_BACKEND").as_deref() == Ok("vulkan") { 0x3450 } else { EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE },'],
  ]);
  patch('hal/gles/src/configs.rs',[
    ['    let maximum = native.iter()', '    // Vulkan supplies real RGB/RGBA configurations; Metal alone needs the\n    // IOSurface-backed opaque alias. Keep the selected provider\'s formats.\n    if std::env::var("KL_ANGLE_BACKEND").as_deref() == Ok("vulkan") { return Some(native); }\n    let maximum = native.iter()'],
  ]);
  patch('crates/aim-host-gpu/src/fence.rs',[
    ['pub fn fence(display: usize) -> Result<OwnedFd, i64> {', 'pub fn fence(display: usize) -> Result<OwnedFd, i64> {\n    if crate::vulkan::selected() { return crate::vulkan::fence(display); }'],
  ]);
}
