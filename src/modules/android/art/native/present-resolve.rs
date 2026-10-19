//! A single-sample intermediate owned by the existing imported-image target.
//! GLES requires identical bounds for an MSAA resolve; orientation and scaling
//! are applied only to this resolved image, never to the multisample source.
use super::Gl;

pub(super) struct Resolve {
    pub(super) framebuffer: u32,
    color: u32,
    width: i32,
    height: i32,
    format: u32,
}

impl Resolve {
    pub(super) unsafe fn retire(self, gl: &Gl) {
        unsafe {
            (gl.delete_framebuffers)(1, &self.framebuffer);
            (gl.delete_renderbuffers)(1, &self.color);
        }
    }
}

/// The original default framebuffer is bound for reading on this context.
pub(super) unsafe fn buffer(gl: &Gl, owned: &mut Option<Resolve>, width: i32, height: i32, source_format: Option<u32>) -> u32 {
    if let Some(current) = owned.as_ref() {
        if (current.width, current.height) == (width, height) { return current.framebuffer; }
    }
    // The resolve attachment must match the source color format, including RGB
    // window configs whose alpha is physically opaque. Query actual attachment
    // precision rather than assuming RGBA8 from a guest config label.
    let format = source_format.unwrap_or_else(|| {
        let attribute = |name| {
            let mut value = 0;
            unsafe { (gl.framebuffer_attachment_parameter)(0x8ca8, 0x0405, name, &mut value) };
            value
        };
        let sizes = (attribute(0x8212), attribute(0x8213), attribute(0x8214), attribute(0x8215));
        let kind = attribute(0x8211);
        color_format(kind, sizes)
    });
    let resolved = owned.get_or_insert_with(|| {
        let (mut framebuffer, mut color) = (0, 0);
        unsafe { (gl.gen_framebuffers)(1, &mut framebuffer); (gl.gen_renderbuffers)(1, &mut color); }
        Resolve { framebuffer, color, width: 0, height: 0, format: 0 }
    });
    if (resolved.width, resolved.height, resolved.format) != (width, height, format) {
        let mut previous = 0;
        unsafe {
            (gl.get_integerv)(0x8ca7, &mut previous); // RENDERBUFFER_BINDING.
            (gl.bind_renderbuffer)(0x8d41, resolved.color);
            (gl.renderbuffer_storage)(0x8d41, format, width, height);
            (gl.bind_renderbuffer)(0x8d41, previous as u32);
            (gl.bind_framebuffer)(0x8ca9, resolved.framebuffer);
            (gl.framebuffer_renderbuffer)(0x8ca9, 0x8ce0, 0x8d41, resolved.color);
        }
        resolved.width = width; resolved.height = height; resolved.format = format;
    }
    resolved.framebuffer
}

/// ES1 has no core attachment-component query. Its original default framebuffer
/// is bound for drawing when these standard precision queries are made.
pub(super) unsafe fn legacy_format(gl: &Gl) -> u32 {
    let mut sizes = [0;4];
    for (value, name) in sizes.iter_mut().zip([0x0d52,0x0d53,0x0d54,0x0d55]) {
        unsafe { (gl.get_integerv)(name, value); }
    }
    color_format(0x1401, (sizes[0],sizes[1],sizes[2],sizes[3]))
}

fn color_format(kind: i32, sizes: (i32,i32,i32,i32)) -> u32 {
    match (kind, sizes) {
        (0x1406, (11, 11, 10, 0)) => 0x8c3a, // R11F_G11F_B10F.
        (0x1406, (16, 16, 16, 0)) => 0x881b, // RGB16F.
        (0x1406, (16, 16, 16, _)) => 0x881a, // RGBA16F.
        (0x1406, (32, 32, 32, 0)) => 0x8815, // RGB32F.
        (0x1406, (32, 32, 32, _)) => 0x8814, // RGBA32F.
        (_, (5, 6, 5, 0)) => 0x8d62, // RGB565.
        (_, (5, 5, 5, 1)) => 0x8057, // RGB5_A1.
        (_, (4, 4, 4, 4)) => 0x8056, // RGBA4.
        (_, (10, 10, 10, 2)) => 0x8059, // RGB10_A2.
        (_, (_, _, _, 0)) => 0x8051, // RGB8.
        _ => 0x8058, // RGBA8.
    }
}
