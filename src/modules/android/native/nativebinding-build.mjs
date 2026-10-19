import path from 'node:path';
import {compileHostLibrary} from './host-compile.mjs';

/** The complete JVM platform library is one incremental native target. */
export function buildNativeBinding({root,build,native,keyMapObject,activityObject,flags,cwd,ninja,python,pathRoots=[]}){
  const inputs=['nativebinding.c','native-methods.c','native-activity.c','framework-input-queue.c','framework-memory.c','framework-appsharedmemory.c','framework-dex.c','framework-native-handle.c','framework-properties.c','network-resolver.c','framework-filesystem.c','framework-zip.c','framework-process.c','framework-message-queue.c','framework-tracing.c','framework-graphics-hints.c','framework-event-log.c','nativeabi.c','nativeabi.S','loader-entry.c','loader-native-registration.c','framework-native-registration.c','loader-class-lookup.c','loader-jni-calls.c','ndk-jni/adapter.c'].map(file=>path.join(root,file))
    .concat(path.join(build,'jni-calls.S'),keyMapObject,activityObject,path.join(build,'host-callbacks.c'));
  return compileHostLibrary({name:'libnativebinding.dylib',inputs,flags,
    extra:['-I'+build,path.join(native,'libklepton-jvm.dylib'),'-lc++',...['Foundation','IOKit','CoreGraphics'].flatMap(name=>['-framework',name])],
    directory:path.join(build,'host-objects'),output:path.join(native,'libnativebinding.dylib'),cwd,ninja,python,pathRoots});
}
