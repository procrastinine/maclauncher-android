import fs from 'node:fs';
import path from 'node:path';
import {copyChanged,writeChanged} from '../native/upstream/source-files.mjs';

export function prepareMulticastOptions(aim){
 const file=path.join(aim,'crates/aim-linux-abi/src/sys/net.rs');
 let text=fs.readFileSync(file,'utf8');
 const changes=[
  ['mod proc_table;', 'mod proc_table;\nmod multicast;'],
  ['    multicast_own: AtomicBool,', '    multicast_own: AtomicBool,\n    /// Linux retains the requested IPv4 multicast address separately from its interface index.\n    multicast_v4_address: std::sync::atomic::AtomicU32,\n    multicast_v6_default: AtomicBool,'],
  ['            w.bool(o.multicast_own.load(Ordering::Relaxed));', '            w.bool(o.multicast_own.load(Ordering::Relaxed));\n            w.u32(o.multicast_v4_address.load(Ordering::Relaxed));\n            w.bool(o.multicast_v6_default.load(Ordering::Relaxed));'],
  ['            o.multicast_own.store(r.bool(), Ordering::Relaxed);', '            o.multicast_own.store(r.bool(), Ordering::Relaxed);\n            o.multicast_v4_address.store(r.u32(), Ordering::Relaxed);\n            o.multicast_v6_default.store(r.bool(), Ordering::Relaxed);'],
  ['    if let Some((l, o)) = int_option(level, opt) {', '    if let Some(result) = multicast::set(&input, level, opt, val, len) { return result; }\n    if let Some((l, o)) = int_option(level, opt) {'],
 ];
 // The plain-int lookup occurs once in each original option entry.
 const plain=changes.pop();
 for(const[before,after]of changes){if(text.split(before).length!==2)throw Error('Original multicast boundary changed');text=text.replace(before,after);}
 const start=text.indexOf('pub fn setsockopt('),end=text.indexOf('\nfn setsockopt_other(',start);
 let body=text.slice(start,end);if(body.split(plain[0]).length!==2)throw Error('Original multicast setter changed');body=body.replace(...plain);text=text.slice(0,start)+body+text.slice(end);
 const getStart=text.indexOf('pub fn getsockopt('),getEnd=text.indexOf('\n    if let Some(s) = any_sock(&input)',getStart);
 if(getStart<0||getEnd<0)throw Error('Original multicast getter changed');
 text=text.slice(0,getEnd)+'\n    if let Some(result) = multicast::get(&input, level, opt, val, len) { return result; }'+text.slice(getEnd);
 writeChanged(file,text);
 copyChanged(path.join(import.meta.dirname,'native/multicast.rs'),path.join(aim,'crates/aim-linux-abi/src/sys/net/multicast.rs'));
}

export function prepareSocketPendingBytes(aim){
 const file=path.join(aim,'crates/aim-linux-abi/src/sys/net.rs');let text=fs.readFileSync(file,'utf8');
 const before='mod proc_table;',after='mod proc_table;\nmod socket_pending;\npub(super) use socket_pending::pending_bytes as pending_datagram_bytes;';
 if(text.split(before).length!==2)throw Error('Original socket module boundary changed');
 writeChanged(file,text.replace(before,after));
 const ioctl=path.join(aim,'crates/aim-linux-abi/src/sys/fs.rs');text=fs.readFileSync(ioctl,'utf8');
 const pending='            FIONREAD => {\n                let n = match inotify::pending_bytes(fd) {';
 if(text.split(pending).length!==2)throw Error('Original FIONREAD boundary changed');
 writeChanged(ioctl,text.replace(pending,`            FIONREAD => {
                // Anonymous Linux descriptors own their ioctl result. Their
                // Darwin socket/kqueue carrier is not a readable byte stream.
                match pin.kind() {
                    Some(Kind::Event(_) | Kind::Timer(_)) => return -(ENOTTY as i64),
                    Some(Kind::Epoll(_)) => return -(EINVAL as i64),
                    _ => {},
                }
                if let Some(result) = net::pending_datagram_bytes(&pin) {
                    return match result {
                        Ok(count) => super::user_memory::write_exact(arg, &count.to_le_bytes())
                            .map(|_| 0).unwrap_or_else(|error| -(error as i64)),
                        Err(error) => -(error as i64),
                    };
                }
                let n = match inotify::pending_bytes(fd) {`));
 copyChanged(path.join(import.meta.dirname,'native/socket-pending.rs'),path.join(aim,'crates/aim-linux-abi/src/sys/net/socket_pending.rs'));
}
