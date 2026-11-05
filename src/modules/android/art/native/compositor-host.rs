//! Native display transport for the Android ART execution backend. Original SurfaceFlinger and the original AIM
//! composer own composition; the existing Mac session/presenter owns its output.
use std::{collections::HashMap,ffi::{CString,c_void},io,os::fd::{AsFd,AsRawFd,IntoRawFd,OwnedFd},os::unix::net::{UnixListener,UnixStream},sync::{Arc,Mutex,mpsc,atomic::{AtomicBool,AtomicU64,Ordering}},time::Duration};
use aim_host_display::unixsock;
use aim_host_display::wire::{self,Request};
use aim_hostcall::display;
use aim_sync_file::{State,Writer};
#[repr(C)] #[derive(Default,Copy,Clone)] struct Output{width:u32,height:u32,dpi_x:u32,dpi_y:u32,interval:u64,frame:u64,presented_ns:u64,completed:u64,screen:u64}
#[repr(C)] #[derive(Default)] struct Tick{sequence:u64,callback:u64,output:u64,interval:u64,coalesced:u64,display:u32}
#[repr(C)] struct Frame{pixels:u64,length:u64,width:u32,height:u32,stride:u32,format:u32,frame:u64,completed:u64,presented:u64,error:[u8;512]}
struct Native{open:unsafe extern "C" fn(*mut Output,i32,i32)->i32,mode:unsafe extern "C" fn(u32,i32,*mut Output)->i32,vsync:unsafe extern "C" fn(i32)->i32,poll:unsafe extern "C" fn(*mut Tick,*mut Output)->i32,submit:unsafe extern "C" fn(*mut Frame,*mut c_void,unsafe extern "C" fn(*mut c_void),*mut c_void,unsafe extern "C" fn(*mut c_void,u64,f64,f64,u32))->i64,close:unsafe extern "C" fn()}
impl Native{unsafe fn load(path:&str)->Self{unsafe{
 let p=CString::new(path).unwrap();let h=libc::dlopen(p.as_ptr(),libc::RTLD_NOW|libc::RTLD_LOCAL);assert!(!h.is_null(),"{}",std::ffi::CStr::from_ptr(libc::dlerror()).to_string_lossy());
 macro_rules! sym{($s:literal)=>{{let p=libc::dlsym(h,concat!($s,"\0").as_ptr().cast());assert!(!p.is_null(),$s);std::mem::transmute(p)}}}
 Self{open:sym!("maclauncher_art_compositor_open"),mode:sym!("maclauncher_art_compositor_mode"),vsync:sym!("maclauncher_art_compositor_vsync"),poll:sym!("maclauncher_art_compositor_poll"),submit:sym!("maclauncher_art_compositor_submit"),close:sym!("maclauncher_art_compositor_close")}
}}}
struct Buffer{info:display::Import,mapping:usize,_file:OwnedFd}
unsafe impl Send for Buffer{} unsafe impl Sync for Buffer{}
impl Buffer{fn new(info:display::Import,file:OwnedFd)->io::Result<Self>{
 let size:usize=info.length.try_into().map_err(|_|io::ErrorKind::InvalidData)?;
 let needed=u64::from(info.stride_bytes)*u64::from(info.height);
 if size==0||info.stride_bytes<u32::checked_mul(info.width,4).ok_or(io::ErrorKind::InvalidData)?||needed>info.length{return Err(io::ErrorKind::InvalidData.into())}
 let mut stat:libc::stat=unsafe{std::mem::zeroed()};if unsafe{libc::fstat(file.as_raw_fd(),&mut stat)}!=0{return Err(io::Error::last_os_error())}if stat.st_size<0||u64::try_from(stat.st_size).unwrap()<info.length{return Err(io::ErrorKind::InvalidData.into())}
 let map=unsafe{libc::mmap(std::ptr::null_mut(),size,libc::PROT_READ,libc::MAP_SHARED,file.as_raw_fd(),0)};if map==libc::MAP_FAILED{return Err(io::Error::last_os_error())}Ok(Self{info,mapping:map as usize,_file:file})
}}
impl Drop for Buffer{fn drop(&mut self){unsafe{libc::munmap(self.mapping as *mut _,self.info.length as usize);}}}
unsafe extern "C" fn released(token:*mut c_void){unsafe{drop(Arc::from_raw(token.cast::<Buffer>()));}}
#[derive(Default)]struct PresentationStats{retired:AtomicU64,presented:AtomicU64,dropped:AtomicU64,cancelled:AtomicU64}
struct Retirement{fence:Writer,stats:Arc<PresentationStats>}
unsafe extern "C" fn retired(token:*mut c_void,frame:u64,shown:f64,when:f64,outcome:u32){let value=unsafe{Box::from_raw(token.cast::<Retirement>())};value.stats.retired.fetch_add(1,Ordering::Relaxed);
 let status=match outcome{1=>{if value.stats.presented.fetch_add(1,Ordering::Relaxed)==0{println!("Original SurfaceFlinger output presented on shared Metal frame={frame} timestamp={shown}");}1},2=>{value.stats.dropped.fetch_add(1,Ordering::Relaxed);1},3=>{value.stats.cancelled.fetch_add(1,Ordering::Relaxed);-125},_=>-5};
 // The native presenter has finished consuming this buffer even when macOS
 // drops an occluded drawable. Keep the real displayed metric separate.
 value.fence.signal_at(((if shown>0.0{shown}else{when})*1e9)as i64,status);
}
enum Command{Client(UnixStream),Mode(u32,u32,u32,mpsc::SyncSender<Option<Output>>),Vsync(bool),Frame(Arc<Buffer>,Writer),Disconnected}
fn receive(stream:UnixStream,tx:mpsc::SyncSender<Command>,output:Arc<Mutex<Output>>)->io::Result<()>{
 struct ClientLease{tx:mpsc::SyncSender<Command>,active:bool}impl Drop for ClientLease{fn drop(&mut self){if self.active{let _=self.tx.send(Command::Disconnected);}}}
 let mut lease=ClientLease{tx:tx.clone(),active:false};
 let mut buffers=HashMap::new();let mut hello=false;
 loop{let mut request=Request::default();let bytes=unsafe{std::slice::from_raw_parts_mut((&mut request as *mut Request).cast(),std::mem::size_of::<Request>())};let mut fds=Vec::new();if !wire::recv(stream.as_fd(),bytes,&mut fds)?{return Ok(())}
 if !hello {
 if request.op==wire::OP_MODE&&request.id==wire::VERSION&&request.flag<=1{
  let(reply,result)=mpsc::sync_channel(1);tx.send(Command::Mode(request.flag,request.import.width,request.import.height,reply)).map_err(|_|io::ErrorKind::BrokenPipe)?;
  let info=result.recv().map_err(|_|io::ErrorKind::BrokenPipe)?.ok_or(io::ErrorKind::InvalidData)?;
  let reply=display::Connect{display:request.flag,width:info.width,height:info.height,dpi_x_milli:info.dpi_x,dpi_y_milli:info.dpi_y,mode:display::mode::DEVICE,vsync_period_ns:info.interval};wire::send(stream.as_fd(),wire::bytes(&reply),None)?;return Ok(())
 }
 if request.op!=wire::OP_HELLO||request.id!=wire::VERSION||request.flag!=0{return Err(io::ErrorKind::InvalidData.into())}
 let output=*output.lock().unwrap();let reply=display::Connect{display:0,width:output.width,height:output.height,dpi_x_milli:output.dpi_x,dpi_y_milli:output.dpi_y,mode:display::mode::DEVICE,vsync_period_ns:output.interval};wire::send(stream.as_fd(),wire::bytes(&reply),None)?;tx.send(Command::Client(stream.try_clone()?)).map_err(|_|io::ErrorKind::BrokenPipe)?;hello=true;lease.active=true;continue;}
 match request.op{
 wire::OP_IMPORT=>{if fds.len()!=1{return Err(io::ErrorKind::InvalidData.into())}let buffer=Buffer::new(request.import,fds.remove(0))?;buffers.insert(request.import.id,Arc::new(buffer));},
 wire::OP_RELEASE=>{buffers.remove(&request.id);},
 wire::OP_SET_VSYNC=>{tx.send(Command::Vsync(request.flag!=0)).map_err(|_|io::ErrorKind::BrokenPipe)?;},
 wire::OP_PRESENT=>{let has_acquire=request.flag&wire::PRESENT_ACQUIRE!=0;if fds.len()!=1+usize::from(has_acquire){return Err(io::ErrorKind::InvalidData.into())}let writer=Writer::from(fds.remove(0));let Some(buffer)=buffers.get(&request.id).cloned()else{return Err(io::ErrorKind::InvalidData.into())};
 if has_acquire{let next=tx.clone();aim_sync_file::on_signal(fds[0].as_fd(),move|state|{match state{State::Signaled{status,..}if status>0=>{let _=next.send(Command::Frame(buffer,writer));},State::Signaled{status,..}=>writer.signal(status),State::Active=>unreachable!()}})?;}else{tx.send(Command::Frame(buffer,writer)).map_err(|_|io::ErrorKind::BrokenPipe)?;}},
 // Device mode uses the native Mac cursor; no Android layer is hidden here.
 wire::OP_CURSOR=>{},
 _=>return Err(io::ErrorKind::Unsupported.into())
 }
 }
}
fn publish_display(socket:&str,output:&Output)->io::Result<()>{
 let file=std::path::Path::new(socket).with_extension("json");let temporary=file.with_extension("json.tmp");
 std::fs::write(&temporary,format!("{{\"width\":{},\"height\":{},\"densityDpi\":{},\"refresh\":{},\"screen\":{}}}",output.width,output.height,output.dpi_x/1000,1_000_000_000.0/output.interval as f64,output.screen))?;std::fs::rename(temporary,file)
}
static STOP:AtomicBool=AtomicBool::new(false);
extern "C" fn stop(_:i32){STOP.store(true,Ordering::Relaxed);}
fn main()->Result<(),Box<dyn std::error::Error>>{
 let args=std::env::args().collect::<Vec<_>>();if args.len()!=3{return Err("art-compositor-host SOCKET SHARED_NATIVE_LIBRARY".into())}
 let browser_path=std::env::var("MACLAUNCHER_ART_BROWSER_SOCKET")?;let browser_path=std::path::Path::new(&browser_path);let browser=unixsock::bind_at(browser_path.parent().ok_or("Missing browser directory")?,browser_path.file_name().and_then(|name|name.to_str()).ok_or("Invalid browser socket name")?,libc::SOCK_STREAM)?;
 let native=unsafe{Native::load(&args[2])};let mut output=Output::default();if unsafe{(native.open)(&mut output,1,browser.into_raw_fd())}!=0{return Err("Cannot open shared Mac presentation session".into())}
 // SDL installs its signal handlers during video initialization. This owner
 // retires the session itself, after the ordinary main-thread polling loop.
 unsafe{libc::signal(libc::SIGTERM,stop as *const () as libc::sighandler_t);libc::signal(libc::SIGINT,stop as *const () as libc::sighandler_t);libc::signal(libc::SIGPIPE,libc::SIG_IGN);}
 publish_display(&args[1],&output)?;let socket=std::path::Path::new(&args[1]);let bound=unixsock::bind_at(socket.parent().ok_or("Missing display directory")?,socket.file_name().and_then(|name|name.to_str()).ok_or("Invalid display socket name")?,libc::SOCK_STREAM)?;if unsafe{libc::listen(bound.as_raw_fd(),8)}!=0{return Err(io::Error::last_os_error().into());}let listener=UnixListener::from(bound);let shared_output=Arc::new(Mutex::new(output));let current_output=shared_output.clone();let(tx,rx)=mpsc::sync_channel(8);std::thread::spawn(move||{for stream in listener.incoming(){match stream{Ok(s)=>{let _=s.set_write_timeout(Some(Duration::from_millis(250)));let next=tx.clone();let current_output=current_output.clone();std::thread::spawn(move||{if let Err(e)=receive(s,next,current_output){eprintln!("composer transport: {e}")}});},Err(e)=>{eprintln!("composer accept: {e}");break;}}}});
 println!("Shared Mac compositor {}x{} {}ns ready",output.width,output.height,output.interval);
 let mut client=None;let mut vsync=false;let stats=Arc::new(PresentationStats::default());let mut submitted=0u64;
 while !STOP.load(Ordering::Relaxed){
 match rx.recv_timeout(Duration::from_millis(2)){
 Ok(Command::Mode(config,width,height,reply))=>{let mut mode=Output::default();let valid=unsafe{(native.mode)(config,0,&mut mode)}==0;
  let query=width==0&&height==0;let commit=valid&&width==mode.width&&height==mode.height&&unsafe{(native.mode)(config,1,&mut mode)}==0;
  let _=reply.send((valid&&(query||commit)).then_some(mode));},
 Ok(Command::Client(s))=>{client=Some(s);},Ok(Command::Vsync(on))=>{vsync=on;if unsafe{(native.vsync)(i32::from(on))}!=0{return Err("Shared display clock failed".into())}},
 Ok(Command::Disconnected)=>{client=None;vsync=false;unsafe{(native.vsync)(0);}},
 Ok(Command::Frame(buffer,writer))=>{let i=buffer.info;let mut frame=Frame{pixels:buffer.mapping as u64,length:i.length,width:i.width,height:i.height,stride:i.stride_bytes/4,format:i.format as u32,frame:0,completed:0,presented:0,error:[0;512]};let token=Arc::into_raw(buffer) as *mut c_void;let observer=Box::into_raw(Box::new(Retirement{fence:writer,stats:stats.clone()}))as *mut c_void;let status=unsafe{(native.submit)(&mut frame,token,released,observer,retired)};if status==0{submitted=frame.frame;}else{let observer=unsafe{released(token);Box::from_raw(observer.cast::<Retirement>())};eprintln!("present rejected: {}",String::from_utf8_lossy(&frame.error).trim_end_matches('\0'));observer.fence.signal(-5);}},
 Err(mpsc::RecvTimeoutError::Timeout)=>{},Err(mpsc::RecvTimeoutError::Disconnected)=>break,
 }
 let mut tick=Tick::default();let observed=unsafe{(native.poll)(&mut tick,&mut output)};if observed<0{break;}let changed={let mut previous=shared_output.lock().unwrap();let changed=previous.width!=output.width||previous.height!=output.height||previous.screen!=output.screen||previous.interval!=output.interval;*previous=output;changed};if changed{publish_display(&args[1],&output)?;}
 if observed>0&&vsync{if let Some(stream)=&client{let event=display::Event{kind:display::event::VSYNC,_reserved:0,timestamp_ns:tick.callback as i64,period_ns:tick.interval as i64,sent_ns:aim_hostcall::clock::monotonic_ns()};if wire::send(stream.as_fd(),wire::bytes(&event),None).is_err(){client=None;vsync=false;}}}
 }
 unsafe{(native.close)();}println!("Shared compositor retired: submitted={} presented={} dropped={} retired={} cancelled={} GPU-completed={}",submitted,stats.presented.load(Ordering::Relaxed),stats.dropped.load(Ordering::Relaxed),stats.retired.load(Ordering::Relaxed),stats.cancelled.load(Ordering::Relaxed),output.completed);Ok(())
}
