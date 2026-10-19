// ANGLE's Vulkan context uses the same original gralloc storage as Metal.
// Vulkan images adopt AIM's existing stride-aware MTLBuffer attachment; ANGLE
// retires the VkImage and its backing after the final GL reference/GPU use.
use std::ffi::{CStr, CString, c_void};
use std::os::fd::OwnedFd;
use std::os::unix::ffi::OsStrExt;
use std::sync::{OnceLock, mpsc};
use crate::{EINVAL, present::entry};
use aim_hostcall::{gpu::ImportBuffer, vulkan::Attach};
#[allow(dead_code)] // The complete original storage module also defines fences.
const EBADF:i64=-9;

#[path = "../../aim-host-vulkan/src/metal.rs"]
#[allow(dead_code)]
mod metal_storage;

#[allow(dead_code)]
struct Private {
    get_mtl_device: usize, set_mtl_texture: usize, pixel_format: usize,
    bytes_per_block: usize, export_objects: usize,
}

pub fn selected() -> bool {
    static SELECTED: OnceLock<bool> = OnceLock::new();
    *SELECTED.get_or_init(|| std::env::var("KL_ANGLE_BACKEND").as_deref() == Ok("vulkan"))
}

unsafe fn symbol<F: Copy>(library: usize, name: &CStr) -> Option<F> {
    let address = unsafe { libc::dlsym(library as *mut c_void, name.as_ptr()) };
    (!address.is_null()).then(|| unsafe { std::mem::transmute_copy(&address) })
}

fn driver() -> Option<&'static Private> {
    static DRIVER: OnceLock<Option<Private>> = OnceLock::new();
    DRIVER.get_or_init(|| {
        let path = CString::new(std::env::var_os("KL_MVK_DYLIB")?.as_bytes()).ok()?;
        let library = unsafe { libc::dlopen(path.as_ptr(), libc::RTLD_NOW | libc::RTLD_LOCAL) } as usize;
        if library == 0 { return None; }
        unsafe {
            Some(Private {
                get_mtl_device: symbol::<usize>(library, c"vkGetMTLDeviceMVK")?,
                set_mtl_texture: symbol::<usize>(library, c"vkSetMTLTextureMVK")?,
                pixel_format: symbol::<usize>(library, c"mvkMTLPixelFormatFromVkFormat")?,
                bytes_per_block: symbol::<usize>(library, c"mvkMTLPixelFormatBytesPerBlock")?,
                export_objects: symbol::<usize>(library, c"vkExportMetalObjectsEXT")?,
            })
        }
    }).as_ref()
}

fn ownership() -> Option<unsafe extern "C" fn(usize, u64, u32) -> bool> {
    static OWNER: OnceLock<Option<unsafe extern "C" fn(usize,u64,u32)->bool>> = OnceLock::new();
    *OWNER.get_or_init(|| {
        let file = crate::LIBRARY_DIR.get()?.join("libGLESv2.dylib");
        let path = CString::new(file.as_os_str().as_bytes()).ok()?;
        let library = unsafe { libc::dlopen(path.as_ptr(), libc::RTLD_NOW | libc::RTLD_LOCAL) } as usize;
        if library == 0 { return None; }
        unsafe { symbol(library, c"kljAngleVkImageOwnership") }
    })
}

#[repr(C)]
#[derive(Clone, Copy)]
struct Extent { width:u32, height:u32, depth:u32 }
#[repr(C)]
#[derive(Clone, Copy)]
struct ImageInfo {
    stype:i32, next:*const c_void, flags:u32, image_type:i32, format:i32,
    extent:Extent, levels:u32, layers:u32, samples:u32, tiling:i32, usage:u32,
    sharing:i32, family_count:u32, families:*const u32, layout:i32,
}
struct Api {
    display:usize, device:usize, physical:usize,queue:usize,
    create_image:unsafe extern "C" fn(usize,*const ImageInfo,*const c_void,*mut u64)->i32,
    destroy_image:unsafe extern "C" fn(usize,u64,*const c_void),
    create_semaphore:unsafe extern "C" fn(usize,*const SemaphoreInfo,*const c_void,*mut u64)->i32,
    destroy_semaphore:unsafe extern "C" fn(usize,u64,*const c_void),
    wait_semaphores:unsafe extern "C" fn(usize,*const SemaphoreWait,u64)->i32,
    create_fence:unsafe extern "C" fn(usize,*const FenceInfo,*const c_void,*mut u64)->i32,
    destroy_fence:unsafe extern "C" fn(usize,u64,*const c_void),
    wait_fences:unsafe extern "C" fn(usize,u32,*const u64,u32,u64)->i32,
    reset_fences:unsafe extern "C" fn(usize,u32,*const u64)->i32,
    submit:unsafe extern "C" fn(usize,u32,*const Submit,u64)->i32,
    lock:unsafe extern "C" fn(usize),unlock:unsafe extern "C" fn(usize),
}

fn api(display:usize) -> Option<Api> {
    // Dispatchable handles are the existing ANGLE driver's, never a second
    // Vulkan instance, device or queue. Resolve through that instance.
    unsafe {
        let query:unsafe extern "C" fn(usize,i32,*mut isize)->u32 = entry(c"eglQueryDisplayAttribEXT")?;
        let attribute:unsafe extern "C" fn(isize,i32,*mut isize)->u32 = entry(c"eglQueryDeviceAttribEXT")?;
        let mut device=0; if query(display,0x322c,&mut device)==0 {return None;}
        let get=|name|{let mut value=0; (attribute(device,name,&mut value)!=0).then_some(value as usize)};
        let instance=get(0x34a9)?;
        let resolve:unsafe extern "C" fn(usize,*const i8)->usize=std::mem::transmute(get(0x34d1)?);
        let load=|name:&CStr|{let address=resolve(instance,name.as_ptr());(address!=0).then_some(address)};
        macro_rules! load {($name:literal)=>{std::mem::transmute(load($name)?)};}
        Some(Api {
            display,device:get(0x34ac)?, physical:get(0x34ab)?,queue:get(0x34af)?,
            create_image:load!(c"vkCreateImage"),destroy_image:load!(c"vkDestroyImage"),
            create_semaphore:load!(c"vkCreateSemaphore"),destroy_semaphore:load!(c"vkDestroySemaphore"),
            wait_semaphores:load!(c"vkWaitSemaphores"),
            create_fence:load!(c"vkCreateFence"),destroy_fence:load!(c"vkDestroyFence"),
            wait_fences:load!(c"vkWaitForFences"),reset_fences:load!(c"vkResetFences"),
            submit:load!(c"vkQueueSubmit"),lock:entry(c"eglLockVulkanQueueANGLE")?,unlock:entry(c"eglUnlockVulkanQueueANGLE")?,
        })
    }
}

pub fn import_buffer(a:&mut ImportBuffer) -> i64 {
    let display=crate::display::host(a.display as usize);
    let (Some(api),Some(driver),Some(ownership))=(api(display),driver(),ownership()) else {
        eprintln!("[android-gles] Vulkan shared images require MoltenVK and the installed Android graphics provider");
        return EINVAL;
    };
    let (format,internal)=match a.format {
        1=>(37,None),2=>(37,Some(0x1907)),4=>(4,None),5=>(44,None),
        0x16=>(97,None),0x2b=>(64,None),0x38=>(9,None),_=>return crate::video::import(a),
    };
    let info=ImageInfo {stype:14,next:std::ptr::null(),flags:0,image_type:1,format,
        extent:Extent{width:a.width,height:a.height,depth:1},levels:1,layers:1,samples:1,
        tiling:0,usage:0x17,sharing:0,family_count:0,families:std::ptr::null(),layout:0};
    let mut image=0;
    unsafe {
        if (api.create_image)(api.device,&info,std::ptr::null(),&mut image)!=0 {return EINVAL;}
        let mut attach=Attach {physical_device:api.physical as u64,image,address:a.address,
            length:a.length,width:a.width,height:a.height,stride_bytes:a.stride_bytes,
            format,..Default::default()};
        if metal_storage::attach(driver,&mut attach)!=0 || attach.result!=0 {
            (api.destroy_image)(api.device,image,std::ptr::null());return EINVAL;
        }
        let Some(create):Option<unsafe extern "C" fn(usize,usize,u32,*const u64,*const i32)->usize>=entry(c"eglCreateImageKHR") else {
            (api.destroy_image)(api.device,image,std::ptr::null());return EINVAL;
        };
        if !ownership(api.device,image,1) {(api.destroy_image)(api.device,image,std::ptr::null());return EINVAL;}
        let pointer=&info as *const _ as u64;
        let mut attributes=vec![0x34d4,(pointer>>32) as i32,0x34d5,pointer as i32];
        if let Some(format)=internal {attributes.extend([0x345d,format]);}
        attributes.push(0x3038);
        let result=create(display,0,0x34d3,&image,attributes.as_ptr());
        if result==0 {
            if ownership(api.device,image,0) {(api.destroy_image)(api.device,image,std::ptr::null());}
            return EINVAL;
        }
        a.image=result as u64;
    }
    0
}

#[repr(C)]
struct SemaphoreType {stype:i32,next:*const c_void,kind:i32,value:u64}
#[repr(C)]
struct ExportMetalObject {stype:i32,next:*const c_void,kind:u32}
#[repr(C)]
struct SemaphoreInfo {stype:i32,next:*const c_void,flags:u32}
#[repr(C)]
struct FenceInfo {stype:i32,next:*const c_void,flags:u32}
#[repr(C)]
struct SemaphoreWait {stype:i32,next:*const c_void,flags:u32,count:u32,semaphores:*const u64,values:*const u64}
#[repr(C)]
struct TimelineSubmit {stype:i32,next:*const c_void,waits:u32,wait_values:*const u64,signals:u32,signal_values:*const u64}
#[repr(C)]
struct Submit {stype:i32,next:*const c_void,waits:u32,wait_semaphores:*const u64,
    wait_stages:*const u32,count:u32,commands:*const usize,signals:u32,signal_semaphores:*const u64}
#[repr(C)]
struct ExportSharedEvent {stype:i32,next:*const c_void,semaphore:u64,event:u64,shared_event:*mut c_void}
#[repr(C)]
struct ExportObjects {stype:i32,next:*mut ExportSharedEvent}

// This state belongs to the existing planar-image lease. The original ANGLE
// image owns the converted RGB texture independently through its last GL ref.
struct VideoInterop {api:Api,semaphores:[u64;2],fence:u64,submitted:bool}
impl Drop for VideoInterop {
    fn drop(&mut self) {unsafe {
        if self.submitted {(self.api.wait_fences)(self.api.device,1,&self.fence,1,u64::MAX);}
        for semaphore in self.semaphores {if semaphore!=0 {(self.api.destroy_semaphore)(self.api.device,semaphore,std::ptr::null());}}
        if self.fence!=0 {(self.api.destroy_fence)(self.api.device,self.fence,std::ptr::null());}
    }}
}

pub(crate) fn video_interop(display:usize)->Option<crate::video::Interop> {
    let api=api(display)?;let driver=driver()?;
    let mut owner=Box::new(VideoInterop{api,semaphores:[0;2],fence:0,submitted:false});
    let mut events=[std::ptr::null_mut();2];
    unsafe {
        let metal_event=ExportMetalObject{stype:1000311000,next:std::ptr::null(),kind:0x20};
        let semaphore_type=SemaphoreType{stype:1000207002,next:&metal_event as *const _ as _,kind:1,value:0};
        let create=SemaphoreInfo{stype:9,next:&semaphore_type as *const _ as _,flags:0};
        let export:unsafe extern "C" fn(usize,*mut ExportObjects)=std::mem::transmute(driver.export_objects);
        for i in 0..2 {
            if (owner.api.create_semaphore)(owner.api.device,&create,std::ptr::null(),&mut owner.semaphores[i])!=0 {return None;}
            let mut event=ExportSharedEvent{stype:1000311010,next:std::ptr::null(),semaphore:owner.semaphores[i],event:0,shared_event:std::ptr::null_mut()};
            let mut info=ExportObjects{stype:1000311001,next:&mut event};export(owner.api.device,&mut info);
            if event.shared_event.is_null(){return None;}events[i]=event.shared_event;
        }
        let fence=FenceInfo{stype:8,next:std::ptr::null(),flags:0};
        if (owner.api.create_fence)(owner.api.device,&fence,std::ptr::null(),&mut owner.fence)!=0{return None;}
        let get:unsafe extern "C" fn(usize,*mut *mut c_void)=std::mem::transmute(driver.get_mtl_device);
        let mut device=std::ptr::null_mut();get(owner.api.physical,&mut device);if device.is_null(){return None;}
        Some(crate::video::Interop{owner:Box::into_raw(owner).cast(),device,ready:events[1],consumed:events[0],
            import_texture:video_texture,begin:video_begin,end:video_end,destroy:video_destroy})
    }
}

unsafe extern "C" fn video_texture(pointer:*mut c_void,texture:*mut c_void,width:u32,height:u32,format:u32)->*mut c_void {
    unsafe {
        let api=&(*(pointer as *mut VideoInterop)).api;
        let (Some(driver),Some(ownership))=(driver(),ownership()) else{return std::ptr::null_mut();};
        let info=ImageInfo{stype:14,next:std::ptr::null(),flags:0,image_type:1,format:format as i32,
            extent:Extent{width,height,depth:1},levels:1,layers:1,samples:1,tiling:0,usage:0x17,
            sharing:0,family_count:0,families:std::ptr::null(),layout:0};
        let mut image=0;if (api.create_image)(api.device,&info,std::ptr::null(),&mut image)!=0{return std::ptr::null_mut();}
        let attach:unsafe extern "C" fn(u64,*mut c_void)->i32=std::mem::transmute(driver.set_mtl_texture);
        if attach(image,texture)!=0 || !ownership(api.device,image,2){(api.destroy_image)(api.device,image,std::ptr::null());return std::ptr::null_mut();}
        let create:Option<unsafe extern "C" fn(usize,usize,u32,*const u64,*const i32)->usize>=entry(c"eglCreateImageKHR");
        let address=&info as *const _ as u64;let attributes=[0x34d4,(address>>32) as i32,0x34d5,address as i32,0x3038];
        let result=create.map_or(0,|create|create(api.display,0,0x34d3,&image,attributes.as_ptr()));
        if result==0 && ownership(api.device,image,0){(api.destroy_image)(api.device,image,std::ptr::null());}
        result as *mut c_void
    }
}

unsafe fn video_submit(owner:&VideoInterop,value:u64,ready:bool,fence:u64)->i32 {
    unsafe {
        let stage=0x10000u32;
        let timeline=TimelineSubmit{stype:1000207003,next:std::ptr::null(),waits:u32::from(ready),
            wait_values:if ready{&value}else{std::ptr::null()},signals:u32::from(!ready),
            signal_values:if ready{std::ptr::null()}else{&value}};
        let submit=Submit{stype:4,next:&timeline as *const _ as _,waits:u32::from(ready),
            wait_semaphores:if ready{&owner.semaphores[1]}else{std::ptr::null()},wait_stages:&stage,
            count:0,commands:std::ptr::null(),signals:u32::from(!ready),
            signal_semaphores:if ready{std::ptr::null()}else{&owner.semaphores[0]}};
        (owner.api.lock)(owner.api.display);let result=(owner.api.submit)(owner.api.queue,1,&submit,fence);(owner.api.unlock)(owner.api.display);result
    }
}
unsafe extern "C" fn video_begin(pointer:*mut c_void,value:u64)->i32 {
    unsafe {
        let owner=&*(pointer as *const VideoInterop);
        if video_submit(owner,value,false,0)!=0{return EINVAL as i32;}
        // CoreImage can upload non-IOSurface source bytes on the CPU. Preserve
        // the existing acquire wait before that API observes the source.
        let wait=SemaphoreWait{stype:1000207004,next:std::ptr::null(),flags:0,count:1,semaphores:&owner.semaphores[0],values:&value};
        if (owner.api.wait_semaphores)(owner.api.device,&wait,u64::MAX)==0{0}else{EINVAL as i32}
    }
}
unsafe extern "C" fn video_end(pointer:*mut c_void,value:u64)->i32 {
    unsafe {
        let owner=&mut *(pointer as *mut VideoInterop);
        if owner.submitted {
            if (owner.api.wait_fences)(owner.api.device,1,&owner.fence,1,u64::MAX)!=0{return EINVAL as i32;}
            owner.submitted=false;
        }
        if (owner.api.reset_fences)(owner.api.device,1,&owner.fence)!=0{return EINVAL as i32;}
        if video_submit(owner,value,true,owner.fence)!=0{return EINVAL as i32;}
        owner.submitted=true;0
    }
}
unsafe extern "C" fn video_destroy(pointer:*mut c_void) {unsafe {drop(Box::from_raw(pointer as *mut VideoInterop));}}

struct FenceWork {display:usize,sync:usize,writer:aim_sync_file::Writer}

fn waiter()->Option<&'static mpsc::SyncSender<FenceWork>> {
    static WAITER:OnceLock<Option<mpsc::SyncSender<FenceWork>>>=OnceLock::new();
    WAITER.get_or_init(|| {
        let (sender,receiver)=mpsc::sync_channel::<FenceWork>(128);
        std::thread::Builder::new().name("gles-vulkan-fences".into()).spawn(move||{
            while let Ok(work)=receiver.recv() {unsafe {
                let pool=crate::metal::pool_push();
                let wait:Option<unsafe extern "C" fn(usize,usize,i32,u64)->i32>=entry(c"eglClientWaitSyncKHR");
                let destroy:Option<unsafe extern "C" fn(usize,usize)->u32>=entry(c"eglDestroySyncKHR");
                let okay=wait.is_some_and(|wait|wait(work.display,work.sync,0,u64::MAX)==0x30f6);
                if let Some(destroy)=destroy {destroy(work.display,work.sync);}
                work.writer.signal(if okay {1}else{-5});
                crate::metal::pool_pop(pool);
            }}
        }).ok()?;
        Some(sender)
    }).as_ref()
}

pub fn fence(display:usize)->Result<OwnedFd,i64> {
    let sender=waiter().ok_or(EINVAL)?;
    unsafe {
        let current:unsafe extern "C" fn()->usize=entry(c"eglGetCurrentContext").ok_or(EINVAL)?;
        if current()==0 {return Err(EINVAL);}
        let create:unsafe extern "C" fn(usize,u32,*const i32)->usize=entry(c"eglCreateSyncKHR").ok_or(EINVAL)?;
        let flush:unsafe extern "C" fn()=entry(c"glFlush").ok_or(EINVAL)?;
        let (file,writer)=aim_sync_file::pair().map_err(|_|EINVAL)?;
        let sync=create(display,0x30f9,[0x3038].as_ptr());
        if sync==0 {return Err(EINVAL);}
        flush();
        if let Err(error)=sender.send(FenceWork{display,sync,writer}) {
            if let Some(destroy)=entry::<unsafe extern "C" fn(usize,usize)->u32>(c"eglDestroySyncKHR") {destroy(display,sync);}
            drop(error);return Err(EINVAL);
        }
        Ok(file)
    }
}
