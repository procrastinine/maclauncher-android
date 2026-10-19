#[path="../../upstream/crates/aim-guest-init/src/paths.rs"] mod paths;
use aim_storage::guest_inode;
#[path="../../upstream/crates/aim-guest-init/src/apex.rs"] mod apex;
#[path="../../upstream/crates/aim-guest-init/src/props.rs"] mod props;
#[path="../../upstream/crates/aim-guest-init/src/propsvc.rs"] mod propsvc;
#[path="../../upstream/crates/aim-guest-init/src/futex.rs"] mod futex;
#[path="../../upstream/crates/aim-guest-init/src/unixsock.rs"] mod unixsock;
#[path="../../upstream/crates/aim-guest-init/src/identity.rs"] mod identity;
use std::{fs,io,os::{fd::AsRawFd,unix::fs::{MetadataExt,OpenOptionsExt,PermissionsExt}},path::{Path,PathBuf}};
use aim_android_init::{image::ImageRoot,props::{PropertyService,load::{create_serialized_property_info,property_init,PropertyInitOptions}}};
#[path="../../upstream/crates/aim-android-image/src/classpath.rs"] mod classpath;

// These are the same published generations and kernel record locks used by
// prepared-cache.js. Native inode metadata is part of installation, so its
// first publication cannot overlap a JVM verifier reading the original APK.
fn preparation_lock(path:&Path)->io::Result<Option<PathBuf>>{
 for root in path.ancestors(){
  if root.parent().and_then(Path::file_name).is_none_or(|name|name!="prepared"){continue;}
  let Some(name)=root.file_name().and_then(|name|name.to_str())else{continue};
  let mut parts=name.split('-');let key=parts.next().unwrap();let suffix=parts.next();
  if key.len()!=64||!key.bytes().all(|b|b.is_ascii_digit()||(b'a'..=b'f').contains(&b))
   ||suffix.is_some_and(|value|value.len()!=16||!value.bytes().all(|b|b.is_ascii_digit()||(b'a'..=b'f').contains(&b)))||parts.next().is_some(){continue;}
  let receipt=fs::symlink_metadata(root.join("prepared.json"))?;
  if !receipt.is_file()||receipt.file_type().is_symlink(){return Err(io::Error::other("Android prepared package metadata is invalid"));}
  return Ok(Some(root.parent().unwrap().parent().unwrap().join(".preparation.lock")));
 }
 Ok(None)
}
fn initialize_packages(inputs:&[String])->io::Result<()>{
 let mut groups=std::collections::BTreeMap::<Option<PathBuf>,Vec<PathBuf>>::new();
 for input in inputs{
  let path=PathBuf::from(input);let paths=groups.entry(preparation_lock(&path)?).or_default();
  if paths.iter().any(|root|path.starts_with(root)){continue;}
  paths.retain(|child|!child.starts_with(&path));paths.push(path);
 }
 for(lock,paths)in groups{
  // One lock at a time avoids ordering dependencies across unrelated packages.
  // The launcher already retains every source generation through this call.
  let guard=if let Some(file)=lock{
   let handle=fs::OpenOptions::new().read(true).write(true).custom_flags(libc::O_NOFOLLOW).open(file)?;
   let metadata=handle.metadata()?;
   if !metadata.is_file()||metadata.uid()!=unsafe{libc::getuid()}||metadata.permissions().mode()&0o077!=0{return Err(io::Error::other("Android preparation lock is invalid"));}
   loop{if unsafe{libc::lockf(handle.as_raw_fd(),libc::F_LOCK,0)}==0{break;}
    let error=io::Error::last_os_error();if error.kind()!=io::ErrorKind::Interrupted{return Err(error);}}
   Some(handle)
  }else{None};
  for path in paths{aim_storage::imported_inodes::initialize_code(&path)?;}
  drop(guard);
 }
 Ok(())
}
fn main()->Result<(),String>{
 let args=std::env::args().collect::<Vec<_>>();
 if args.len()==3&&args[1]=="wait-binder"{
  let deadline=std::time::Instant::now()+std::time::Duration::from_secs(5);
  loop{if let Ok(port)=aim_binder_host::mach::look_up(&args[2]){aim_binder_host::mach::release_send(port);return Ok(());}
   if std::time::Instant::now()>=deadline{return Err("Native Binder registration did not complete".into());}
   std::thread::sleep(std::time::Duration::from_millis(10));}
 }
 if args.len()>=3&&args[1]=="initialize-package"{
  initialize_packages(&args[2..]).map_err(|e|e.to_string())?;
  return Ok(());
 }
 if args.len()>=4&&args[1]=="initialize"{
  let uid=args[2].parse::<u32>().map_err(|e|e.to_string())?;
  for root in &args[3..]{aim_storage::imported_inodes::initialize(std::path::Path::new(root),uid).map_err(|e|e.to_string())?;}
  return Ok(());
 }
 if args.len()==4&&args[1]=="layout"{
  let root=PathBuf::from(&args[2]);let out=PathBuf::from(&args[3]);let layout=paths::Layout::new(root.clone(),out.join("data"),Some(out.join("run")));
  layout.prepare().map_err(|e|e.to_string())?;
  fs::write(layout.apex_info_list(),apex::apex_info_list_xml(&apex::scan(&ImageRoot::new(&root)))).map_err(|e|e.to_string())?;
  print!("{}",layout.path_map().to_file_text());return Ok(());
 }
if args.len()==5&&args[1]=="serve"{return serve(&args[2],&args[3],&args[4]);}if args.len()!=3{return Err("art-bootstrap-inputs IMAGE PROPERTY_DIRECTORY".into());}
 let image=ImageRoot::new(&args[1]);let out=PathBuf::from(&args[2]);fs::create_dir_all(&out).map_err(|e|e.to_string())?;
 let mut diagnostics=Vec::new();let options=PropertyInitOptions::default();
 let info=create_serialized_property_info(&image,options.vendor_api_level,&mut diagnostics)?;
 let mut properties=PropertyService::new(info)?;diagnostics.extend(property_init(&mut properties,&image,&options));
 if !properties.init_set("ro.hardware.egl","aim").is_success(){return Err("Cannot select original AIM EGL driver".into());}
 for (key,value) in [("ro.opengles.version","196608"),("sys.use_memfd","true"),("debug.renderengine.backend","skiaglthreaded"),("ro.sf.lcd_density","320"),("debug.sf.nobootanimation","1"),("ro.surface_flinger.max_frame_buffer_acquired_buffers","3"),("debug.sf.enable_gl_backpressure","0"),("debug.sf.vsync_reactor_ignore_present_fences","false")] {
  if !properties.init_set(key,value).is_success(){return Err(format!("Cannot select original physical graphics property {key}"));}
 }
 // Static launch inputs use the original property serializer. No property
 // service daemon or alternative Android service implementation is started.
 let areas=properties.areas();let mut files=vec![("property_info",areas.property_info().to_vec()),("properties_serial",areas.serial_area().bytes())];
 for context in areas.contexts(){files.push((context,areas.area(context).unwrap().bytes()));}
 for(name,data)in files{let file=out.join(name);fs::write(&file,data).map_err(|e|e.to_string())?;fs::set_permissions(file,fs::Permissions::from_mode(0o444)).map_err(|e|e.to_string())?;}
 let oat=fs::read(image.host_path("/system/framework/arm64/boot.oat")).map_err(|e|e.to_string())?;let key=b"\0bootclasspath\0";let at=oat.windows(key.len()).position(|w|w==key).ok_or("missing original boot classpath")?+key.len();let end=at+oat[at..].iter().position(|b|*b==0).ok_or("unterminated original boot classpath")?;
 let mut jars=String::from_utf8(oat[at..end].to_vec()).map_err(|e|e.to_string())?.split(':').map(str::to_owned).collect::<Vec<_>>();
 for jar in classpath::jars(image.root(),"bootclasspath.pb",classpath::BOOTCLASSPATH)?{if !jars.contains(&jar){jars.push(jar);}}
 println!("{}",jars.join(":"));Ok(())
}

fn serve(image:&str,root:&str,by_pid:&str)->Result<(),String>{
 use crate::{props::mapped_properties,propsvc::{PropertySockets,PropertyEvent},identity::Identity};
 use aim_android_init::props::{Ucred,protocol::{serve,PROP_ERROR_PERMISSION_DENIED},load::start_property_service};
 let image=ImageRoot::new(image);let root=PathBuf::from(root);let directory=root.join("properties");fs::create_dir_all(&directory).map_err(|e|e.to_string())?;
 let mut diagnostics=Vec::new();let options=PropertyInitOptions::default();let info=create_serialized_property_info(&image,options.vendor_api_level,&mut diagnostics)?;
 let mut properties=mapped_properties(&directory,info)?;property_init(&mut properties,&image,&options);
 for(key,value)in [("apex.all.ready","true"),("ro.hardware.egl","aim"),("ro.opengles.version","196608"),("sys.use_memfd","true"),("debug.renderengine.backend","skiaglthreaded"),("ro.sf.lcd_density","320"),("debug.sf.nobootanimation","1"),("ro.surface_flinger.max_frame_buffer_acquired_buffers","3"),("debug.sf.enable_gl_backpressure","0"),("debug.sf.vsync_reactor_ignore_present_fences","false")] {if !properties.init_set(key,value).is_success(){return Err(format!("Cannot set {key}"));}}
 start_property_service(&mut properties);
 let sockets=root.join("sockets");fs::create_dir_all(&sockets).map_err(|e|e.to_string())?;let(tx,rx)=std::sync::mpsc::channel();let _sockets=PropertySockets::start(&sockets,tx).map_err(|e|e.to_string())?;println!("Original native property service ready");
 for event in rx {let PropertyEvent::Set(request)=event else{continue};
  let identity=Identity::read_entry(&std::path::Path::new(by_pid).join(request.peer_pid.to_string()));
  let Some(identity)=identity.filter(|i|i.uid==1000&&request.peer_host_uid==unsafe{libc::getuid()})else{request.reply(PROP_ERROR_PERMISSION_DENIED);continue};
  let cred=Ucred{pid:request.peer_pid,uid:identity.uid,gid:identity.gid};let result=serve(&mut properties,&request.request,Some(&identity.seclabel),&cred);if let Some(code)=result.reply{request.reply(code);}
 }
 Ok(())
}
