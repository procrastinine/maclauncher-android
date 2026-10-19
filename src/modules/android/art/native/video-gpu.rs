//! Original EGL/native-buffer lifetime with the shared Mac video conversion.
use std::ffi::{CStr, c_char, c_void};
use aim_hostcall::gpu::{ImportBuffer, VideoBuffer, VideoImage};
use crate::{display, metal, resolved, EINVAL};
unsafe extern "C" {
    fn klj_video_image_create(buffer: *const VideoBuffer, device: *mut c_void,
        display: *mut c_void, resolve: unsafe extern "C" fn(*const c_char)->*mut c_void,
        image: *mut *mut c_void) -> *mut c_void;
    fn klj_video_image_refresh(owner: *mut c_void, dataspace: i32) -> i32;
    fn klj_video_image_destroy(owner: *mut c_void);
    fn klj_video_image_create_interop(buffer:*const VideoBuffer,display:*mut c_void,
        resolve:unsafe extern "C" fn(*const c_char)->*mut c_void,interop:*const Interop,
        image:*mut *mut c_void)->*mut c_void;
}
#[repr(C)]
pub(crate) struct Interop {
    pub owner:*mut c_void,pub device:*mut c_void,pub ready:*mut c_void,pub consumed:*mut c_void,
    pub import_texture:unsafe extern "C" fn(*mut c_void,*mut c_void,u32,u32,u32)->*mut c_void,
    pub begin:unsafe extern "C" fn(*mut c_void,u64)->i32,
    pub end:unsafe extern "C" fn(*mut c_void,u64)->i32,
    pub destroy:unsafe extern "C" fn(*mut c_void),
}
unsafe extern "C" fn resolve(name: *const c_char)->*mut c_void {
    unsafe { resolved(CStr::from_ptr(name)) as *mut c_void }
}
pub fn import(a: &mut ImportBuffer)->i64 {
    let display=display::host(a.display as usize);
    let source=VideoBuffer{address:a.address,length:a.length,width:a.width,height:a.height,
        fourcc:a.fourcc,plane_count:a.plane_count,planes:a.planes};
    let mut image=std::ptr::null_mut();
    let lease=if crate::vulkan::selected() {
        let Some(interop)=crate::vulkan::video_interop(display) else {return EINVAL};
        unsafe {klj_video_image_create_interop(&source,display as *mut c_void,resolve,&interop,&mut image)}
    } else {
        let Some(device)=metal::device(display) else {return EINVAL};
        unsafe {klj_video_image_create(&source,device,display as *mut c_void,resolve,&mut image)}
    };
    if lease.is_null(){return EINVAL;}
    a.lease=lease as u64;a.image=image as u64;0
}
pub fn update(a: &mut VideoImage)->i64 {
    if a.lease==0{return EINVAL;}
    match a.operation {
        0=>unsafe {klj_video_image_refresh(a.lease as *mut c_void,a.dataspace) as i64},
        1=>{unsafe {klj_video_image_destroy(a.lease as *mut c_void)};0},
        _=>EINVAL,
    }
}
