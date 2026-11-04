import fs from 'node:fs';
import path from 'node:path';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';

/** Keep the original ELF translator and file mapping owner across loaders
 * which populate anonymous reservations with positional reads. */
export function prepareElfCopies(aim){
 const root=path.join(aim,'crates/aim-linux-abi/src');
 const edit=(file,changes)=>{const target=path.join(root,file);let source=fs.readFileSync(target,'utf8');
  for(const[from,to]of changes){if(source.split(from).length!==2)throw Error('AIM ELF-copy boundary changed: '+file+' '+from.slice(0,60));source=source.replace(from,to);}
  writeChanged(target,source);
 };
 copyChanged(path.join(import.meta.dirname,'native/elf-copies.rs'),path.join(root,'sys/elf-copies.rs'));
 edit('sys/copies.rs',[
  ['    pub ino: u64,','    pub ino: u64,\n    pub(super) source: Option<Arc<VerifiedSource>>,'],
  ['            ino,\n        },','            ino,\n            source: None,\n        },'],
  ['        w.u64(c.ino);','        w.u64(c.ino);\n        save_source(w,c.source.as_ref());'],
  ['                ino: r.u64(),','                ino: r.u64(),\n                source: restore_source(r),'],
  ['static COPIES:', 'include!("elf-copies.rs");\n\nstatic COPIES:'],
 ]);
 edit('xrt.rs',[
  ['fn archive_sites(fd: i32, st: &FileStat, off: u64) -> Option<Arc<Sites>> {','fn archive_member(fd: i32, st: &FileStat, off: u64) -> Option<(u64,u64)> {'],
  ['    let (start, size) = zip::stored_at(&members, off, read)?;','    zip::stored_at(&members, off, read)\n}\n\nfn archive_sites(fd: i32, st: &FileStat, off: u64) -> Option<Arc<Sites>> {\n    let (start, size) = archive_member(fd,st,off)?;'],
  ['/// How an executable private mapping of `fd` at file offset `off` is made.',`/// Original ELF extent for a direct file or a stored APK member. The same
/// archive cache serves mmap and positional-copy loaders.
pub(crate) fn elf_extent(fd:i32,off:u64)->Option<(u64,u64)>{
    let (st,_)=FileStat::of_fd(fd).ok()?;
    let head=read_fd(fd,0,20)?;
    if xlate::elf::is_aarch64_elf(&head){return Some((0,st.size));}
    if head.get(..4)!=Some(b"PK\\x03\\x04"){return None;}
    let extent=archive_member(fd,&st,off)?;
    xlate::elf::is_aarch64_elf(&read_fd(fd,extent.0,20)?).then_some(extent)
}

/// How an executable private mapping of \`fd\` at file offset \`off\` is made.`],
 ]);
 edit('sys/fs.rs',[
  ['        let result = description.rw(pin.descriptor(), &one(buf, len), Some(pos), write);',
   '        let result = description.rw(pin.descriptor(), &one(buf, len), Some(pos), write);\n        if !write&&result>0 {super::copies::note_elf_read(&pin,buf,result as u64,pos as u64);}'],
  ['    Some(\n        special_pio_kernel(actual, buf, len, pos, write).unwrap_or_else(|| {',
   '    let result = special_pio_kernel(actual, buf, len, pos, write).unwrap_or_else(|| {'],
  ['        }),\n    )\n}\n\nfn special_read_kernel',
   '        });\n    if !write&&result>0 {super::copies::note_elf_read(&pin,buf,result as u64,pos as u64); }\n    Some(result)\n}\n\nfn special_read_kernel'],
 ]);
 edit('sys/mem.rs',[
  [`                let stats=match patch::rewrite_region(lo,hi-lo){Ok(stats)=>stats,Err(error)=>{
                    host_mprotect(lo,hi-lo,cur_prot);
                    return -(errno::from_darwin(error.raw_os_error().unwrap_or(libc::EIO))as i64);
                }};
                trace_stats("rewrote", lo, hi - lo, &stats);`, `                let known=copies::file_copies(lo,hi);
                let result=(||->Result<(),i64>{
                    let scan=|begin:u64,end:u64|->Result<(),i64>{
                        let begin=(begin+3)&!3;let end=end&!3;
                        if begin<end {let stats=patch::rewrite_region(begin,end-begin).map_err(|e|-(errno::from_darwin(e.raw_os_error().unwrap_or(libc::EIO))as i64))?;trace_stats("rewrote",begin,end-begin,&stats);}
                        Ok(())
                    };
                    let mut cursor=lo;
                    for (start,copy) in known {
                        let begin=start.max(lo);let end=copy.end.min(hi);
                        scan(cursor,begin)?;
                        use std::os::fd::AsRawFd;
                        let fd=copy.source.as_ref().unwrap().as_raw_fd();
                        let offset=copy.offset+begin-start;
                        rewrite_file_copy(begin,end-begin,fd,offset,&xrt::exec_source(fd,offset))?;
                        copies::forget_elf_sites(begin,end);
                        cursor=end;
                    }
                    scan(cursor,hi)?;
                    Ok(())
                })();
                if let Err(error)=result {
                    host_mprotect(lo,hi-lo,cur_prot);return error;
                }
                `],
 ]);
 // Fork state is private to the same executable; its mapping records now carry
 // a retained source descriptor as well as the already preserved inode/path.
 const state=path.join(root,'sys/fork/state.rs'),text=fs.readFileSync(state,'utf8');
 if(text.split('0x46444e10').length!==4)throw Error('AIM fork state version boundary changed');
 writeChanged(state,text.replaceAll('0x46444e10','0x46444e11'));
}
