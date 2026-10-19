import fs from 'node:fs';
import path from 'node:path';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';
const root=import.meta.dirname;
/** Keep the original gralloc layout, image registry, external-texture binding
 * and EGL destruction. The host operation uses the same physical conversion
 * source as decoded video on the other VM. */
export function prepareAimVideo(aim){
 const edit=(file,changes)=>{const target=path.join(aim,file);let text=fs.readFileSync(target,'utf8');for(const[from,to]of changes){if(text.split(from).length!==2)throw new Error('AIM video source differs: '+file+' '+from.slice(0,80));text=text.replace(from,to);}writeChanged(target,text);};
 const gpu='crates/aim-host-gpu';
 for(const name of ['video-image.mm','video-image.h','video-pixels.mm','video-pixels.hpp','angle-metal.hpp'])copyChanged(path.join(root,'../native',name),path.join(aim,gpu,'src/mac-video',name));
 copyChanged(path.join(root,'native/video-build.rs'),path.join(aim,gpu,'build.rs'));
 copyChanged(path.join(root,'native/video-gpu.rs'),path.join(aim,gpu,'src/video.rs'));
 edit(gpu+'/src/lib.rs',[
  ['mod metal;','mod metal;\nmod video;'],
  ['        FN_IMPORT_BUFFER => match',`        aim_hostcall::gpu::FN_VIDEO_IMAGE => match unsafe { args_mut::<aim_hostcall::gpu::VideoImage>(args, len) } {
            Ok(a) => video::update(a), Err(e) => e,
        },
        FN_IMPORT_BUFFER => match`],
 ]);
 edit(gpu+'/src/metal.rs',[[`    let Some((pixel_format, bpp, internal)) = metal_format(a.format) else {
        return EINVAL;
    };`,`    let Some((pixel_format, bpp, internal)) = metal_format(a.format) else {
        return crate::video::import(a);
    };`]]);
 edit(gpu+'/tests/angle.rs',[[`        format: 1,
        image: 0,
    };`,`        format: 1,
        image: 0,
        ..Default::default()
    };`]]);
 edit('crates/aim-hostcall/src/lib.rs',[
  ['pub const VERSION: u32 = 3;\n\n    /// Load ANGLE','pub const VERSION: u32 = 4;\n\n    /// Load ANGLE'],
  ['    pub const FN_OPAQUE_SURFACE: u32 = 5;','    pub const FN_OPAQUE_SURFACE: u32 = 5;\n    pub const FN_VIDEO_IMAGE: u32 = 6;'],
  ['core::mem::size_of::<ImportBuffer>() == 48','core::mem::size_of::<ImportBuffer>() == 160'],
  ['        pub image: u64,\n    }\n\n    /// Argument block of [`FN_PRESENT`].',`        pub image: u64,
        pub fourcc: u32,
        pub plane_count: u32,
        pub planes: [VideoPlane; 3],
        pub lease: u64,
    }
    #[repr(C)]
    #[derive(Clone, Copy, Debug, Default)]
    pub struct VideoPlane {pub offset:u64,pub stride:u64,pub width:u64,pub height:u64}
    #[repr(C)]
    pub struct VideoBuffer {
        pub address:u64,pub length:u64,pub width:u32,pub height:u32,pub fourcc:u32,pub plane_count:u32,
        pub planes:[VideoPlane;3],
    }
    #[repr(C)]
    pub struct VideoImage {pub lease:u64,pub dataspace:i32,pub operation:u32}

    /// Argument block of FN_PRESENT.`],
  ['    pub fn gpu_import_buffer(args: &mut gpu::ImportBuffer)', '    pub fn gpu_video_image(args: &mut gpu::VideoImage) -> Result<(), Errno> {call_with(module::GPU,gpu::FN_VIDEO_IMAGE,args).map(drop)}\n\n    pub fn gpu_import_buffer(args: &mut gpu::ImportBuffer)'],
 ]);
 edit('hal/gles/src/buffers.rs',[
  ['use aim_hostcall::gpu::ImportBuffer;','use aim_hostcall::gpu::{ImportBuffer, VideoPlane, VideoImage};'],
  ['    pub image: EGLImage,','    pub image: EGLImage,\n    pub lease: u64,\n    pub metadata: usize,'],
  ['    let length = h.metadata_offset as usize;','    let length = h.total_size() as usize;'],
  ['        image: 0,','        image: 0,\n        fourcc: aim_gralloc::layout::fourcc(h.format),\n        plane_count: layout.planes.len() as u32,\n        planes: std::array::from_fn(|i|layout.planes.get(i).map_or(VideoPlane::default(),|p|VideoPlane{offset:p.offset,stride:p.stride_bytes,width:p.width_samples,height:p.height_samples})),\n        lease: 0,'],
  ['        image: args.image as EGLImage,','        image: args.image as EGLImage,\n        lease: args.lease,\n        metadata: address + h.metadata_offset as usize,'],
  ['/// Drop one import\'s hold on the buffer\'s mapping (its image destroyed).',`/// Refresh only planar images when the original consumer binds an acquired image.
pub fn refresh(lease:u64,metadata:usize)->bool {
    if lease==0{return true;}
    let data=unsafe {&*(metadata as *const aim_gralloc::metadata::SharedMetadata)};
    let mut request=VideoImage{lease,dataspace:if data.is_valid(){data.dataspace}else{0},operation:0};
    aim_hostcall::guest::gpu_video_image(&mut request).is_ok()
}
pub fn retire(lease:u64){
    if lease!=0 {let mut request=VideoImage{lease,dataspace:0,operation:1};let _=aim_hostcall::guest::gpu_video_image(&mut request);}
}

/// Drop one import's hold on the buffer's mapping (its image destroyed).`],
 ]);
 edit('hal/gles/src/egl.rs',[
  ["fn native_images() -> MutexGuard<'static, HashMap<usize, u64>> {\n    static IMAGES: OnceLock<Mutex<HashMap<usize, u64>>> = OnceLock::new();", `struct NativeImage {id:u64,lease:u64,metadata:usize}
impl Drop for NativeImage {fn drop(&mut self){buffers::retire(self.lease);buffers::unmap(self.id);}}
fn native_images() -> MutexGuard<'static, HashMap<usize, std::sync::Arc<NativeImage>>> {
    static IMAGES: OnceLock<Mutex<HashMap<usize, std::sync::Arc<NativeImage>>>> = OnceLock::new();`],
  ['native_images().insert(i.image as usize, i.id);','native_images().insert(i.image as usize, std::sync::Arc::new(NativeImage{id:i.id,lease:i.lease,metadata:i.metadata}));'],
  ['fn destroyed(image: EGLImage) {\n    if let Some(id) = native_images().remove(&(image as usize)) {\n        buffers::unmap(id);',`pub(crate) fn refresh_native_image(image:EGLImage)->bool {
    let owner={native_images().get(&(image as usize)).cloned()};
    match owner {Some(i)=>buffers::refresh(i.lease,i.metadata),None=>true}
}
fn destroyed(image: EGLImage) {
    let owner={native_images().remove(&(image as usize))};
    if let Some(owner) = owner {
        drop(owner);`],
 ]);
 edit('hal/gles/src/external.rs',[[
  'pub unsafe extern "C" fn glEGLImageTargetTexture2DOES(target: GLenum, image: GLeglImageOES) {',
  'pub unsafe extern "C" fn glEGLImageTargetTexture2DOES(target: GLenum, image: GLeglImageOES) {\n    if !crate::egl::refresh_native_image(image) {return;}',
 ]]);
}
