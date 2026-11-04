import fs from 'node:fs';
import path from 'node:path';
import {execFileSync,spawnSync} from 'node:child_process';
import {pathToFileURL} from 'node:url';
const [sourceRoot,adapterFile,outputRoot]=process.argv.slice(2);
const adapter=await import(pathToFileURL(path.resolve(adapterFile)));
const originalHost=fs.readFileSync(path.join(sourceRoot,'crates/aim-host-gpu/src/fence.rs'),'utf8');
const originalGuest=fs.readFileSync(path.join(sourceRoot,'hal/gles/src/sync.rs'),'utf8');
function method(text,marker){const start=text.indexOf(marker),open=text.indexOf('{',start);if(start<0)throw Error(marker);let depth=1,end=open+1;while(depth){if(text[end]==='{')depth++;if(text[end]==='}')depth--;end++;}return text.slice(start,end);}
const support=String.raw`#![allow(dead_code,unused_imports,unused_variables)]
extern crate self as aim_sync_file;
extern crate self as aim_hostcall;
use std::sync::atomic::{AtomicBool,AtomicI32,AtomicUsize,Ordering};
static FAIL_SYNC:AtomicBool=AtomicBool::new(false);
static FAIL_EVENT:AtomicBool=AtomicBool::new(false);
static EGL_ERROR:AtomicI32=AtomicI32::new(0);
static GUEST_ERROR:AtomicI32=AtomicI32::new(0);
static LAST_ERROR:AtomicI32=AtomicI32::new(0);
static REGISTERED:AtomicUsize=AtomicUsize::new(0);
const EINVAL:i64=-22;
pub mod errno{pub const ENOMEM:i32=12;}
pub mod gpu{pub struct Fence{pub display:u64,}}
pub mod guest{pub struct Errno(pub i32);pub fn gpu_fence(_: &mut crate::gpu::Fence)->Result<i32,Errno>{Err(Errno(crate::GUEST_ERROR.load(crate::Ordering::Relaxed)))}}
#[macro_export]
macro_rules! send {($device:expr,$selector:expr => $ret:ty)=>{crate::metal::new_event()};}
pub mod metal{
 pub(crate) use crate::send;pub type Id=*mut std::ffi::c_void;
 pub fn device(_:usize)->Option<Id>{Some(1usize as Id)}
 pub fn new_event()->Id{if crate::FAIL_EVENT.load(crate::Ordering::Relaxed){std::ptr::null_mut()}else{1usize as Id}}
 pub unsafe fn objc_release(_:Id){}
 pub struct CompletionObservation;
 pub unsafe fn observe_completion(_:Id,_:u64)->std::io::Result<CompletionObservation>{Ok(CompletionObservation)}
 impl CompletionObservation{pub fn detach(&self){} pub fn fence(&self)->std::io::Result<std::os::fd::OwnedFd>{unsafe{fence(std::ptr::null_mut(),1)}}}
 pub unsafe fn fence(_:Id,_:u64)->std::io::Result<std::os::fd::OwnedFd>{crate::REGISTERED.fetch_add(1,crate::Ordering::Relaxed);Ok(std::fs::File::open("/dev/null")?.into())}
}
pub mod vulkan{pub fn selected()->bool{false}pub fn fence(_:usize)->Result<std::os::fd::OwnedFd,i64>{unreachable!()}}
unsafe extern "C" fn current()->usize{1}
unsafe extern "C" fn create(_:usize,_:u32,_:*const isize)->usize{if FAIL_SYNC.load(Ordering::Relaxed){0}else{1}}
unsafe extern "C" fn destroy(_:usize,_:usize)->u32{1}
unsafe extern "C" fn wait(_:usize,_:usize,_:i32)->u32{1}
unsafe extern "C" fn flush(){}
unsafe extern "C" fn error()->u32{EGL_ERROR.load(Ordering::Relaxed)as u32}
pub mod present{
 pub unsafe fn entry<T:Copy>(name:&std::ffi::CStr)->Option<T>{let ptr=match name.to_bytes(){b"eglGetCurrentContext"=>crate::current as *const (),b"eglCreateSync"=>crate::create as *const (),b"eglDestroySync"=>crate::destroy as *const (),b"eglWaitSync"=>crate::wait as *const (),b"eglGetCurrentDisplay"=>crate::current as *const (),b"glFlush"=>crate::flush as *const (),b"eglGetError"=>crate::error as *const (),_=>return None};Some(unsafe{std::mem::transmute_copy(&ptr)})}
}
`;
const guestSupport=String.raw`
mod guest_sync{
use std::collections::HashMap;
use std::os::fd::{FromRawFd,OwnedFd};
use std::sync::{Mutex,MutexGuard,OnceLock};
use crate::gpu::Fence;
type EGLDisplay=*mut std::ffi::c_void;type EGLSync=*mut std::ffi::c_void;type EGLint=i32;
const EGL_NO_NATIVE_FENCE_FD_ANDROID:i32=-1;const EGL_SYNC_NATIVE_FENCE_FD_ANDROID:i32=0x3145;const EGL_BAD_MATCH:i32=0x3009;const EGL_BAD_ATTRIBUTE:i32=0x3004;
struct NativeSync(OwnedFd);
fn natives()->MutexGuard<'static,HashMap<usize,Box<NativeSync>>>{static MAP:OnceLock<Mutex<HashMap<usize,Box<NativeSync>>>>=OnceLock::new();MAP.get_or_init(Default::default).lock().unwrap()}
fn fail(error:i32,result:EGLSync)->EGLSync{crate::LAST_ERROR.store(error,crate::Ordering::Relaxed);result}
`;
const main=String.raw`
fn main(){
 FAIL_SYNC.store(true,Ordering::Relaxed);EGL_ERROR.store(0x3003,Ordering::Relaxed);
 let host_alloc=host::fence(1).unwrap_err()==-12;
 GUEST_ERROR.store(12,Ordering::Relaxed);guest_sync::create_native(std::ptr::null_mut(),&[]);
 let guest_alloc=LAST_ERROR.load(Ordering::Relaxed)==0x3003;
 EGL_ERROR.store(0x3009,Ordering::Relaxed);let mismatch=host::fence(1).unwrap_err()==-22;
 GUEST_ERROR.store(22,Ordering::Relaxed);guest_sync::create_native(std::ptr::null_mut(),&[]);
 let guest_mismatch=LAST_ERROR.load(Ordering::Relaxed)==0x3009;
 FAIL_EVENT.store(true,Ordering::Relaxed);let event_alloc=host::fence(1).unwrap_err()==-12;
 let no_pending=REGISTERED.load(Ordering::Relaxed)==0;
 FAIL_EVENT.store(false,Ordering::Relaxed);FAIL_SYNC.store(false,Ordering::Relaxed);
 let valid=host::fence(1).is_ok()&&REGISTERED.load(Ordering::Relaxed)==1;
 println!("hostAllocation={} guestAllocation={} mismatchPreserved={} nilEventAllocation={} noPendingWriter={} valid={}",host_alloc,guest_alloc,mismatch&&guest_mismatch,event_alloc,no_pending,valid);
 if !(host_alloc&&guest_alloc&&mismatch&&guest_mismatch&&event_alloc&&no_pending&&valid){std::process::exit(1)}
}
`;
fs.mkdirSync(outputRoot,{recursive:true});const results=[];
for(const [name,hostSource,guestSource]of [['baseline',originalHost,originalGuest],['fixed',adapter.adaptNativeFenceAllocationErrors(originalHost),adapter.adaptGuestFenceAllocationErrors(originalGuest)]]){
 const input=path.join(outputRoot,name+'.rs'),binary=path.join(outputRoot,name);
 const host=hostSource.replace(method(hostSource,'pub fn wait('),'').replace(/^\/\/!.*$/gm,'');
 const guest=method(guestSource,'fn create_native(').replace('fn create_native(','pub fn create_native(');
 fs.writeFileSync(input,support+'\nmod host{\n'+host+'\n}\n'+guestSupport+guest+'\n}\n'+main);
 execFileSync(process.env.RUSTC||'rustc',['--edition=2024',input,'-o',binary],{stdio:'inherit'});
 const result=spawnSync(binary,{encoding:'utf8'});results.push({name,status:result.status,stdout:result.stdout,stderr:result.stderr});console.log(name+': '+result.stdout.trim());
}
fs.writeFileSync(path.join(outputRoot,'result.json'),JSON.stringify({sourceRoot:path.resolve(sourceRoot),adapter:path.resolve(adapterFile),results},null,2)+'\n');
if(results[0].status!==1||results[1].status!==0)throw Error('Expected original RED/fixed GREEN');
