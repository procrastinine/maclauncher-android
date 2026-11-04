import fs from 'node:fs';
import path from 'node:path';
import {copyChanged,writeChanged} from '../upstream/source-files.mjs';

/** Add finite, same-physical-thread Bionic entries to the original Linux ABI.
 * Requires the existing guest-callback and shared process identity adaptations.
 * No source/default selection is changed by this preparation helper. */
export function prepareBionicThreads(aim){
const output=path.join(aim,'crates/aim-linux-abi'),work=import.meta.dirname;
function edit(relative,pairs){const file=path.join(output,'src',relative);let text=fs.readFileSync(file,'utf8');for(const[from,to]of pairs){if(text.split(from).length!==2)throw Error('Original Bionic thread boundary changed: '+relative);text=text.replace(from,to);}writeChanged(file,text);}
edit('context.rs',[
 ['    pub callback_sp: u64,','    pub callback_sp: u64,\n    pub(crate) foreign_frame: u64,\n    pub(crate) foreign_reason: u64,'],
 ['    fn linux_abi_call_guest(ctx: *mut GuestContext, function: u64, argument: u64) -> u64;','    fn linux_abi_call_guest(ctx: *mut GuestContext, function: u64, argument: u64) -> u64;\n    pub(crate) fn klj_bionic_enter_context(ctx: *mut GuestContext) -> u64;']
]);
writeChanged(path.join(output,'src/context.rs'),fs.readFileSync(path.join(output,'src/context.rs'),'utf8')+'\n'+fs.readFileSync(path.join(work,'thread-binding.rs'),'utf8')+'\nglobal_asm!(include_str!("thread-admission.S"),foreign_frame=const std::mem::offset_of!(GuestContext,foreign_frame),foreign_reason=const std::mem::offset_of!(GuestContext,foreign_reason),in_host=const std::mem::offset_of!(GuestContext,in_host),host_sp=const std::mem::offset_of!(GuestContext,host_sp));\n');
// Retain the VM caller's physical JIT view across returning guest entries and
// every synchronous Java-capable host call. The original probe supplies state.
edit('context.rs', [['        linux_abi_call_guest(ctx, function, argument)','        let _view = ForeignJitView::capture().expect("guest callback JIT-view capture");\n        linux_abi_call_guest(ctx, function, argument)']]);
edit('hostcall.rs',[
 ['        return unsafe { call(func, args, len, crate::context::call_guest) };','        let _view=match crate::context::ForeignJitView::host(){Ok(view)=>view,Err(error)=>return -(error as i64)};\n        return unsafe { call(func, args, len, crate::context::call_guest) };'],
 ['            return unsafe { call(0x10000 + func, args, len, crate::context::call_guest) };','            let _view=match crate::context::ForeignJitView::host(){Ok(view)=>view,Err(error)=>return -(error as i64)};\n            return unsafe { call(0x10000 + func, args, len, crate::context::call_guest) };']
]);
edit('sys/jit-user-copy.rs',[['fn protected()->Result<bool,Errno>{','pub(crate) fn protected()->Result<bool,Errno>{']]);

// Feed the one additional original-layout field to the existing trampoline.
edit('context.rs', [['    callback_sp = const std::mem::offset_of!(GuestContext, callback_sp),','    callback_sp = const std::mem::offset_of!(GuestContext, callback_sp),\n    foreign_reason = const std::mem::offset_of!(GuestContext, foreign_reason),']]);
edit('trampoline.S',[['_linux_abi_check:\n','_linux_abi_check:\n    ldr x0, [x19, #{foreign_reason}]\n    cbz x0, Lbionic_no_return\n    b _klj_bionic_return_context\nLbionic_no_return:\n']]);
edit('hostcall.rs',[['        if let Err(error)=crate::process_identity::install(handle){libc::dlclose(handle);return Err(error);}','        if let Err(error)=crate::process_identity::install(handle){libc::dlclose(handle);return Err(error);}\n        crate::sys::thread::admission::install(handle);']]);
edit('sys/mod.rs',[
 ['mod thread;','pub(crate) mod thread;'],
 ['pub fn dispatch(ctx: &mut GuestContext) {','pub fn dispatch(ctx: &mut GuestContext) {\n    if ctx.x[8]==93 && thread::admission::is_current(){ctx.x[0]=thread::admission::exit_guest([ctx.x[0],0,0,0,0,0]) as u64;return;}'],
 ['        93 => thread::exit(a),','        93 => thread::exit(a),\n        thread::admission::YIELD_NR => thread::admission::yield_guest(ctx,a),']
]);
const threadFile=path.join(output,'src/sys/thread.rs');let thread=fs.readFileSync(threadFile,'utf8');
const start=thread.indexOf('pub fn exit(a: [u64; 6]) -> ! {'),end=thread.indexOf('\npub fn exit_group(',start);if(start<0||end<0)throw Error('Original exit boundary absent');
const originalExit=thread.slice(start,end),cut=originalExit.indexOf('    // SAFETY: nothing runs on this thread');if(cut<0)throw Error('Original physical retirement boundary absent');
let common=originalExit.slice(0,cut).replace('pub fn exit(a: [u64; 6]) -> ! {\n    let code = a[0] as i32 & 0xff;','fn finish_thread_exit(code:i32,may_end_process:bool)->(*mut GuestContext,Option<HostStacks>,bool){').replace('    if left == 0 {','    if left == 0 && may_end_process {').replace('exit_group(a)','exit_group([code as u64,0,0,0,0,0])');
const physical=originalExit.slice(cut);const replacement=common+'    (ctx,stacks,main)\n}\npub fn exit(a:[u64;6])->!{\n    let(ctx,stacks,main)=finish_thread_exit(a[0]as i32&0xff,true);\n'+physical;
thread=thread.slice(0,start)+replacement+thread.slice(end);
const claimAnchor='    let traced = super::ptrace::clone_event(flags);';if(thread.split(claimAnchor).length!==2)throw Error('Original clone claim boundary absent');thread=thread.replace(claimAnchor,claimAnchor+'\n    let admission=match admission::claim_clone(){Ok(value)=>value,Err(error)=>return error};');
const idAnchor=`        let reservation = alloc_tid(t)?;
        let tid = reservation.tid;`;if(thread.split(idAnchor).length!==2)throw Error('Original clone ID boundary absent');thread=thread.replace(idAnchor,`        let (tid,reservation)=match admission::borrowed_tid(admission.as_ref(),t){
            Ok(Some(id))=>(id,None),
            Ok(None)=>{let reservation=alloc_tid(t)?;(reservation.tid,Some(reservation))},
            Err(error)=>return Err((-error)as crate::errno::Errno),
        };`);
thread=thread.replace('            Some(reservation),','            reservation,');
thread=thread.replace('        Err(error) => {\n            // SAFETY: never handed out.','        Err(error) => {\n            admission::failed_clone(admission.as_ref());\n            // SAFETY: never handed out.');
const marker='    // The child may run, exit and be freed before pthread_create returns:';if(thread.split(marker).length!==2)throw Error('Original clone host choice absent');thread=thread.replace(marker,'    if let Some(result)=unsafe{admission::capture(boot,flags,admission)}{return result;}\n'+marker);
thread+='\n#[path="thread-admission.rs"]\npub(crate) mod admission;\n';writeChanged(threadFile,thread);
for(const [from,to]of [['thread-admission.rs','sys/thread-admission.rs'],['thread-admission.S','thread-admission.S']])copyChanged(path.join(work,from),path.join(output,'src',to));

}
