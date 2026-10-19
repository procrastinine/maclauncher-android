//! Opaque native descriptor attachments use the existing fd entry/lifecycle.
use std::{io::{self,Write},os::fd::{AsRawFd,FromRawFd,OwnedFd},sync::Arc};
use aim_ashmem::capability;
use aim_storage::private_fd::PrivateFd;
use crate::errno;

pub(super) struct Access {handle:i64,token:Vec<u8>,proof:PrivateFd}
impl Drop for Access {fn drop(&mut self){capability::klj_ashmem_capability_release(self.handle);}}
fn error(error:io::Error)->i32 {errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))}
fn proof(bytes:&[u8])->io::Result<PrivateFd>{
    PrivateFd::allocate(||{
        let mut path=std::env::temp_dir().join("aim-ashmem-access.XXXXXX").into_os_string().into_encoded_bytes();path.push(0);
        let fd=unsafe{libc::mkstemp(path.as_mut_ptr().cast())};if fd<0{return Err(io::Error::last_os_error());}
        let mut writer=unsafe{std::fs::File::from_raw_fd(fd)};
        let result=(||{writer.write_all(bytes)?;let reader=unsafe{libc::open(path.as_ptr().cast(),libc::O_RDONLY|libc::O_CLOEXEC)};
            if reader<0{Err(io::Error::last_os_error())}else{Ok(unsafe{OwnedFd::from_raw_fd(reader)})}})();
        let removed=unsafe{libc::unlink(path.as_ptr().cast())};
        if removed<0{return Err(io::Error::last_os_error());}result
    })
}
impl Access {
    fn new(fd:i32,token:Vec<u8>)->Result<Arc<Self>,i32>{
        let lease=capability::consume(fd,&token).map_err(error)?;
        let proof=proof(&token).map_err(error)?;
        Ok(Arc::new(Self{handle:lease.into_raw(),token,proof}))
    }
}
fn same_file(a:i32,b:i32)->bool{
    let(mut x,mut y):(libc::stat,libc::stat)=unsafe{std::mem::zeroed()};
    unsafe{libc::fstat(a,&mut x)==0&&libc::fstat(b,&mut y)==0&&x.st_dev==y.st_dev&&x.st_ino==y.st_ino}
}
fn attach(fd:i32,access:Arc<Access>){
    let previous=super::TABLE.write().unwrap().entry(fd).or_default().access.replace(access);
    super::retire_entry(previous.map(|access|super::Entry{access:Some(access),..Default::default()}));
    super::set_slow(fd,true);
}
pub(crate) fn import(fd:i32,token:Vec<u8>)->Result<(),i32>{
    let pin=super::pin_guest_io(fd)?;
    let access=Access::new(pin.descriptor().as_raw_fd(),token)?;
    let guard=super::lifecycle();
    super::require_guest_visible(fd)?;
    if !same_file(fd,pin.descriptor().as_raw_fd()){return Err(errno::EBADF);}
    attach(fd,access);super::publish_guest(fd)?;
    drop(guard);drop(pin);Ok(())
}

#[repr(C)]
#[derive(Clone,Copy)]
struct Packet {version:u32,operation:u32,fd:i32,reserved:u32,bytes:u64,size:u64}
pub(crate) fn call(address:u64,length:u64)->i64{
    let result=(||->Result<i64,i32>{
        if length!=std::mem::size_of::<Packet>()as u64{return Err(errno::EINVAL);}
        let bytes=super::super::user_memory::read_exact(address,length as usize)?;
        let packet=unsafe{bytes.as_ptr().cast::<Packet>().read_unaligned()};
        if packet.version!=1||packet.reserved!=0{return Err(errno::EINVAL);}
        match packet.operation {
            0=>{let pin=super::pin_guest_io(packet.fd)?;let token=capability::token(pin.descriptor().as_raw_fd()).map_err(error)?;
                if packet.bytes!=0 {if token.len()as u64>packet.size{return Err(errno::ERANGE);}super::super::user_memory::write_exact(packet.bytes,&token)?;}
                Ok(token.len()as i64)},
            1=>{let size=usize::try_from(packet.size).map_err(|_|errno::ENOMEM)?;
                let token=super::super::user_memory::read_exact(packet.bytes,size)?;
                import(packet.fd,token)?;Ok(0)},
            _=>Err(errno::EINVAL),
        }
    })();match result{Ok(value)=>value,Err(error)=>-(error as i64)}
}

pub(super) fn save(table:&std::collections::HashMap<i32,super::Entry>,writer:&mut super::super::fork_state::Writer){
    let entries=table.iter().filter(|(fd,_)|super::visible(**fd)).filter_map(|(fd,entry)|entry.access.as_ref().map(|access|(*fd,access))).collect::<Vec<_>>();
    let mut objects=Vec::<*const Access>::new();
    writer.seq(entries.into_iter(),|writer,(fd,access)|{writer.i32(fd);let pointer=Arc::as_ptr(access);
        if let Some(index)=objects.iter().position(|old|*old==pointer){writer.u64(index as u64);writer.bool(false);}
        else{writer.u64(objects.len()as u64);objects.push(pointer);writer.bool(true);writer.bytes(&access.token);}});
}
pub(super) fn restore(reader:&mut super::super::fork_state::Reader){
    let mut objects=Vec::<Arc<Access>>::new();
    let entries=reader.seq(|reader|{let fd=reader.i32();let index=reader.u64()as usize;
        let access=if reader.bool(){let token=reader.bytes();match Access::new(fd,token){Ok(access)=>{objects.push(access.clone());Some(access)},Err(_)=>{reader.invalidate();None}}}
            else{objects.get(index).cloned()};if access.is_none(){reader.invalidate();}(fd,access)});
    for(fd,access)in entries{if let Some(access)=access{attach(fd,access);}}
}
pub(super) fn exec_record(fd:i32,access:&Arc<Access>)->String{
    format!("ashmem-access\t{fd}\t{}\t{}",access.proof.as_raw_fd(),access.handle)
}
pub(super) fn proof_flags(access:&Arc<Access>)->Result<(i32,i32),i32>{
    let fd=access.proof.as_raw_fd();let flags=unsafe{libc::fcntl(fd,libc::F_GETFD)};
    if flags<0{return Err(errno::last());}if unsafe{libc::fcntl(fd,libc::F_SETFD,flags&!libc::FD_CLOEXEC)}<0{return Err(errno::last());}Ok((fd,flags))
}
pub(super) fn restore_exec(fd:i32,carrier:i32,old_handle:i64,objects:&mut Vec<(i32,Arc<Access>)>)->Result<(),i32>{
    super::require_guest_visible(fd)?;
    let access=if let Some((_,access))=objects.iter().find(|(old,_)|*old==carrier){access.clone()}
    else{
        if carrier<0||super::visible(carrier)||old_handle<0{return Err(errno::EPERM);}
        let mut stat:libc::stat=unsafe{std::mem::zeroed()};
        if unsafe{libc::fstat(carrier,&mut stat)}<0{return Err(errno::last());}
        let flags=unsafe{libc::fcntl(carrier,libc::F_GETFL)};
        if flags<0{return Err(errno::last());}
        if flags&libc::O_ACCMODE!=libc::O_RDONLY||stat.st_size<=0{return Err(errno::EINVAL);}
        let size=usize::try_from(stat.st_size).map_err(|_|errno::ENOMEM)?;
        let mut token=Vec::new();token.try_reserve_exact(size).map_err(|_|errno::ENOMEM)?;token.resize(size,0);let mut count=0;
        while count<token.len(){let read=unsafe{libc::pread(carrier,token[count..].as_mut_ptr().cast(),token.len()-count,count as i64)};
            if read<0{return Err(errno::last());}if read==0{return Err(errno::EIO);}count+=read as usize;}
        let access=Access::new(fd,token)?;
        // Host exec preserves this process's old native authority. Transfer its
        // ownership only after the replacement lease is verified and retained.
        capability::klj_ashmem_capability_release(old_handle);
        objects.push((carrier,access.clone()));access
    };
    // Duplicate guest aliases must identify the same marked anonymous inode.
    if capability::token(fd).map_err(error)?!=access.token{return Err(errno::EINVAL);}
    attach(fd,access);super::publish_guest(fd)
}
