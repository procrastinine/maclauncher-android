//! Bionic's normal loader may have a smaller RELRO alignment than its LOAD
//! alignment. Retain writable data sharing the final physical page; complete
//! RELRO pages still use the original mprotect owner. No mapping state is kept.
use std::{ffi::CString,os::{fd::AsRawFd,unix::ffi::OsStrExt},sync::Arc};
use crate::{context,elf,vfs,xrt};
use super::super::{copies,verified_source::VerifiedSource,vmmap};

fn linker_call() -> bool {
    let ctx=context::current_ctx();
    if ctx.is_null(){return false;}
    // The saved return address belongs to the caller of Bionic's syscall stub.
    let caller=unsafe{(*ctx).x[30]};
    let Some(region)=vmmap::region_at(caller) else{return false;};
    if caller<region.start || caller>=region.end || region.prot&4==0{return false;}
    let guest=if let Some((copy,_))=copies::find(caller){Some(copy.guest)}else{
        region.file.and_then(|(host,_,_)|xrt::original_guest_path_of_host(&host).or_else(||vfs::guest_path_of_host(&host)))
    };
    matches!(guest.as_deref(),Some("/apex/com.android.runtime/bin/linker64"|"/system/bin/linker64"))
}

fn adapted(a:[u64;6])->Option<[u64;6]>{
    let page=super::PAGE;
    if a[2]!=libc::PROT_READ as u64 || a[0]&(page-1)!=0 || a[1]==0 || !linker_call(){return None;}
    let region=vmmap::region_at(a[0])?;
    if region.start>a[0] || region.end<=a[0] || region.shared{return None;}
    let (host,dev,ino,offset,mapped,retained)=if let Some((copy,offset))=copies::find(a[0]){
        (copy.host,copy.dev,copy.ino,offset,a[0],copy.source)
    }else{
        let(host,dev,ino)=region.file?;(host,dev,ino,region.offset,region.start,None)
    };
    let source=if let Some(source)=retained{source}else{
        Arc::new(VerifiedSource::open(&CString::new(host.as_os_str().as_bytes()).ok()?,false).ok()?)
    };
    let identity=source.identity();
    if identity.dev!=dev || identity.ino!=ino{return None;}
    let (extent,size)=xrt::elf_extent(source.as_raw_fd(),offset)?;
    let mut head=[0u8;64];source.read_exact_at(&mut head,extent).ok()?;
    let header=elf::parse_header(&head).ok()?;
    let end=header.e_phoff.checked_add(header.e_phnum as u64*header.e_phentsize as u64)?;
    if end>size || end>65536{return None;}
    let mut bytes=vec![0;end.max(64)as usize];source.read_exact_at(&mut bytes,extent).ok()?;
    let headers=elf::parse_phdrs(&bytes,&header).ok()?;
    for load in &headers {
        if load.p_type!=elf::PT_LOAD || load.p_flags&elf::PF_W==0 {continue;}
        let file_start=extent.checked_add(load.p_offset&!(page-1))?;
        let Some(delta)=offset.checked_sub(file_start)else{continue;};
        let virtual_start=(load.p_vaddr&!(page-1)).checked_add(delta)?;
        let Some(bias)=mapped.checked_sub(virtual_start)else{continue;};
        for relro in &headers {
            if relro.p_type!=0x6474e552{continue;}
            let lo=bias.checked_add(relro.p_vaddr)?;
            let hi=lo.checked_add(relro.p_memsz)?;
            let rounded_hi=hi.checked_add(page-1)?&!(page-1);
            if lo&!(page-1)!=a[0] || rounded_hi.checked_sub(a[0])?!=a[1] || hi==rounded_hi {continue;}
            // A partial trailing native page is relaxed only when the same
            // original writable LOAD contains bytes outside the RELRO range.
            let data_end=bias.checked_add(load.p_vaddr)?.checked_add(load.p_memsz)?;
            if bias.checked_add(load.p_vaddr)?>hi || data_end<=hi {continue;}
            let mut result=a;result[1]=(hi&!(page-1)).checked_sub(a[0])?;
            return Some(result);
        }
    }
    None
}

pub(super) fn request(a:[u64;6])->Option<[u64;6]>{adapted(a)}
