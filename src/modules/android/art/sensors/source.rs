//! Read-only representation of the shared HostSensors source. No controls or
//! motion calculations live in the HAL process.
use std::{fs::File,io::{self,Read},os::fd::AsRawFd,path::Path,sync::atomic::{AtomicU64,Ordering},time::Duration};
use android_hardware_sensors::aidl::android::hardware::sensors::{SensorInfo::SensorInfo,SensorType::SensorType,SensorStatus::SensorStatus,Event::EventPayload::{EventPayload,Vec3::Vec3,Data::Data}};
#[derive(Clone)]
struct Description{handle:i32,kind:i32,version:i32,min:i32,max:i32,flags:i32,range:f32,resolution:f32,power:f32,encoding:i32,name:String,vendor:String,kind_name:String,permission:String}
pub struct Source{descriptions:Vec<Description>,address:*mut libc::c_void,length:usize}
// The file is a read-only process-shared mapping. A release/acquire sequence
// brackets each producer update; samples are copied only between equal epochs.
unsafe impl Send for Source{}
unsafe impl Sync for Source{}
impl Drop for Source{fn drop(&mut self){unsafe{libc::munmap(self.address,self.length);}}}
fn integer(input:&mut impl Read)->io::Result<i32>{let mut data=[0;4];input.read_exact(&mut data)?;Ok(i32::from_be_bytes(data))}
fn float(input:&mut impl Read)->io::Result<f32>{Ok(f32::from_bits(integer(input)? as u32))}
fn text(input:&mut impl Read,remaining:&mut usize)->io::Result<String>{let length=usize::try_from(integer(input)?).map_err(|_|io::Error::from(io::ErrorKind::InvalidData))?;if length>*remaining{return Err(io::ErrorKind::UnexpectedEof.into());}*remaining-=length;let mut bytes=vec![0;length];input.read_exact(&mut bytes)?;String::from_utf8(bytes).map_err(|_|io::ErrorKind::InvalidData.into())}
impl Source{
 pub fn open(directory:&Path)->io::Result<Self>{
  let mut file=loop{match File::open(directory.join("descriptors")){Ok(file)=>break file,Err(error)if error.kind()==io::ErrorKind::NotFound=>std::thread::sleep(Duration::from_millis(5)),Err(error)=>return Err(error)}};
  let mut magic=[0;8];file.read_exact(&mut magic)?;if &magic!=b"MLSENS01"{return Err(io::ErrorKind::InvalidData.into());}
  let count=usize::try_from(integer(&mut file)?).map_err(|_|io::Error::from(io::ErrorKind::InvalidData))?;
  let mut remaining=usize::try_from(file.metadata()?.len()).map_err(|_|io::Error::from(io::ErrorKind::InvalidData))?;
  if count>remaining/56{return Err(io::ErrorKind::InvalidData.into());}
  let mut descriptions=Vec::with_capacity(count);
  for _ in 0..count{descriptions.push(Description{handle:integer(&mut file)?,kind:integer(&mut file)?,version:integer(&mut file)?,min:integer(&mut file)?,max:integer(&mut file)?,flags:integer(&mut file)?,range:float(&mut file)?,resolution:float(&mut file)?,power:float(&mut file)?,encoding:integer(&mut file)?,name:text(&mut file,&mut remaining)?,vendor:text(&mut file,&mut remaining)?,kind_name:text(&mut file,&mut remaining)?,permission:text(&mut file,&mut remaining)?});}
  let data=File::open(directory.join("values"))?;let length=count.checked_mul(64).and_then(|n|n.checked_add(16)).ok_or(io::ErrorKind::InvalidData)?;
  if data.metadata()?.len()!=length as u64{return Err(io::ErrorKind::InvalidData.into());}
  let address=unsafe{libc::mmap(std::ptr::null_mut(),length,libc::PROT_READ,libc::MAP_SHARED,data.as_raw_fd(),0)};
  if address==libc::MAP_FAILED{return Err(io::Error::last_os_error());}Ok(Self{descriptions,address,length})
 }
 pub fn list(&self)->Vec<SensorInfo>{self.descriptions.iter().map(|d|SensorInfo{sensorHandle:d.handle,name:d.name.clone(),vendor:d.vendor.clone(),version:d.version,r#type:SensorType(d.kind),typeAsString:d.kind_name.clone(),maxRange:d.range,resolution:d.resolution,power:d.power,minDelayUs:d.min,fifoReservedEventCount:0,fifoMaxEventCount:0,requiredPermission:d.permission.clone(),maxDelayUs:d.max,flags:d.flags}).collect()}
 pub fn snapshot(&self)->Option<(i64,Vec<[f32;16]>)>{
  let sequence=unsafe{&*self.address.cast::<AtomicU64>()};
  for _ in 0..3{let before=sequence.load(Ordering::Acquire);if before==0||before&1!=0{continue;}
   let timestamp=unsafe{std::ptr::read_volatile(self.address.cast::<i64>().add(1))};
   let mut rows=Vec::with_capacity(self.descriptions.len());for i in 0..self.descriptions.len(){let mut values=[0.;16];for(j,value)in values.iter_mut().enumerate(){*value=unsafe{std::ptr::read_volatile(self.address.cast::<u8>().add(16+i*64+j*4).cast::<f32>())};}rows.push(values);}
   std::sync::atomic::fence(Ordering::Acquire);if before==sequence.load(Ordering::Acquire){return Some((timestamp,rows));}
  }None
 }
 pub fn payload(&self,index:usize,values:[f32;16])->EventPayload{match self.descriptions[index].encoding{0=>EventPayload::Vec3(Vec3{x:values[0],y:values[1],z:values[2],status:SensorStatus::ACCURACY_HIGH}),_=>EventPayload::Data(Data{values})}}
}
