import fs from 'node:fs';
import path from 'node:path';
import {writeChanged} from '../native/upstream/source-files.mjs';

export function adaptNativeFenceAllocationErrors(source) {
  const replace = (before, after) => {
    if (source.split(before).length !== 2) throw Error('ART native fence allocation source changed');
    source = source.replace(before, after);
  };
  replace('const EGL_NONE: isize = 0x3038;',
    'const EGL_NONE: isize = 0x3038;\nconst EGL_BAD_ALLOC: u32 = 0x3003;\nconst ENOMEM: i64 = -(aim_hostcall::errno::ENOMEM as i64);');
  replace('    flush: unsafe extern "C" fn(),',
    '    flush: unsafe extern "C" fn(),\n    get_error: unsafe extern "C" fn() -> u32,');
  replace('                flush: entry(c"glFlush")?,',
    '                flush: entry(c"glFlush")?,\n                get_error: entry(c"eglGetError")?,');
  const at = source.indexOf('pub fn fence('), prefix = source.slice(0, at);
  source = source.slice(at);
  replace('    if event.is_null() {\n        return Err(EINVAL);\n    }',
    '    if event.is_null() {\n        return Err(ENOMEM);\n    }');
  replace('    if sync == 0 {\n        // SAFETY: our reference from newSharedEvent.',
    '    if sync == 0 {\n        // SAFETY: read the error of this failed EGL call on the same thread.\n        let error = unsafe { (egl.get_error)() };\n        // SAFETY: our reference from newSharedEvent.');
  replace('        unsafe { objc_release(event) };\n        return Err(EINVAL);',
    '        unsafe { objc_release(event) };\n        return Err(if error == EGL_BAD_ALLOC { ENOMEM } else { EINVAL });');
  replace('    let attribs = [',
    '    let observation = match unsafe { aim_sync_file::metal::observe_completion(event, 1) } {\n        Ok(value) => value,\n        Err(_) => { unsafe { objc_release(event) }; return Err(ENOMEM); }\n    };\n    let attribs = [');
  replace('    if sync == 0 {', '    observation.detach();\n    if sync == 0 {');
  replace('    let file = unsafe { aim_sync_file::metal::fence(event, 1) };',
    '    let file = observation.fence();');
  return prefix + source;
}

export function adaptGuestFenceAllocationErrors(source) {
  const before = '            Err(_) => return fail(EGL_BAD_MATCH, std::ptr::null_mut()),';
  if (source.split(before).length !== 2) throw Error('ART guest fence allocation source changed');
  return adaptGuestFenceWaitErrors(source.replace(before, `            Err(error) => return fail(
                if error.0 == aim_hostcall::errno::ENOMEM { 0x3003 } else { EGL_BAD_MATCH },
                std::ptr::null_mut()),`));
}

export function prepareNativeFenceAllocationErrors(root) {
  for (const [file, adapt] of [
    ['crates/aim-host-gpu/src/fence.rs', adaptNativeFenceAllocationErrors],
    ['hal/gles/src/sync.rs', adaptGuestFenceAllocationErrors],
  ]) {
    const target = path.join(root, file);
    writeChanged(target, adapt(fs.readFileSync(target, 'utf8')));
  }
  const metal = path.join(root, 'crates/aim-sync-file/src/metal.rs');
  writeChanged(metal, adaptMetalFenceCompletion(fs.readFileSync(metal, 'utf8')));
  writeChanged(path.join(path.dirname(metal), 'maclauncher-fence-completion.rs'),
    fs.readFileSync(new URL('./native/metal-fence-completion.rs', import.meta.url)));
  writeChanged(path.join(root, 'hal/gles/src/maclauncher-fence-wait.rs'),
    fs.readFileSync(new URL('./native/guest-fence-wait.rs', import.meta.url)));
}

export function adaptMetalFenceCompletion(source) {
  const replace = (before, after) => {
    if (source.split(before).length !== 2) throw Error('Metal native fence completion source changed');
    source = source.replace(before, after);
  };
  replace('        fn objc_release(obj: Id);',
    '        fn objc_release(obj: Id);\n        fn objc_setAssociatedObject(obj: Id, key: Sel, value: Id, policy: usize);');
  replace('pub unsafe fn fence(event: Id, value: u64) -> io::Result<OwnedFd> {',
    'unsafe fn fence_impl(event: Id, value: u64, completion: Option<&CompletionState>) -> io::Result<OwnedFd> {');
  replace('    // SAFETY: caller contract; the reference lasts until the event\n    // reaches `value`.\n    unsafe { objc_retain(event) };\n    PENDING\n        .lock()\n        .unwrap()\n        .push((event as usize, value, writer));',
    '    if !register_pending(event, value, writer, completion) { return Ok(file); }');
  return source + '\ninclude!("maclauncher-fence-completion.rs");\n';
}


export function adaptGuestFenceWaitErrors(source) {
  const replace = (before, after, count = 1) => {
    if (source.split(before).length !== count + 1) throw Error('Guest native fence wait source changed');
    source = source.replaceAll(before, after);
  };
  const marker = 'fn poll(fd: i32, timeout: u64) -> bool {';
  const begin = source.indexOf(marker);
  if (begin < 0) throw Error('Guest native fence poll source changed');
  let end = source.indexOf('{', begin) + 1, depth = 1;
  while (depth && end < source.length) {
    if (source[end] === '{') depth++;
    if (source[end] === '}') depth--;
    end++;
  }
  if (depth) throw Error('Guest native fence poll is unterminated');
  source = source.slice(0, begin) + 'include!("maclauncher-fence-wait.rs");' + source.slice(end);
  replace('fn native_attrib(fd: i32, attr: EGLint) -> Option<EGLint> {\n    Some(match attr {',
    'fn native_attrib(fd: i32, attr: EGLint) -> Result<Option<EGLint>, EGLint> {\n    Ok(Some(match attr {');
  replace('        EGL_SYNC_STATUS if poll(fd, 0) => EGL_SIGNALED,\n        EGL_SYNC_STATUS => EGL_UNSIGNALED,\n        _ => return None,\n    })',
    '        EGL_SYNC_STATUS => if poll(fd, 0)? { EGL_SIGNALED } else { EGL_UNSIGNALED },\n        _ => return Ok(None),\n    }))');
  replace('        None => fail(EGL_BAD_ATTRIBUTE, EGL_FALSE),\n        Some(_) if value.is_null() => fail(EGL_BAD_PARAMETER, EGL_FALSE),\n        Some(v) => {',
    '        Err(error) => fail(error, EGL_FALSE),\n        Ok(None) => fail(EGL_BAD_ATTRIBUTE, EGL_FALSE),\n        Ok(Some(_)) if value.is_null() => fail(EGL_BAD_PARAMETER, EGL_FALSE),\n        Ok(Some(v)) => {', 2);
  replace('    // SAFETY: a query of this thread\'s current context.\n    if unsafe { host::eglGetCurrentContext() }.is_null()',
    '    // Report a failed sync_file without queuing a GPU dependency.\n    if let Err(error) = poll(fd, 0) { return fail(error, EGL_FALSE); }\n    // SAFETY: a query of this thread\'s current context.\n    if unsafe { host::eglGetCurrentContext() }.is_null()');
  replace('        return if poll(fd, timeout) {\n            EGL_CONDITION_SATISFIED\n        } else {\n            EGL_TIMEOUT_EXPIRED\n        };',
    '        return match poll(fd, timeout) {\n            Ok(true) => EGL_CONDITION_SATISFIED,\n            Ok(false) => EGL_TIMEOUT_EXPIRED,\n            Err(error) => fail(error, EGL_FALSE as EGLint),\n        };');
  return source;
}
