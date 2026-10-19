import path from 'node:path';
import {prepareWebkitCallbacks} from './webkit-jni-source.mjs';
import {prepareGlesGuestAbi} from './gles-guest-abi.mjs';

// Full builds and component rebuilds share the complete object/dependency list.
// Compiler callbacks retain the caller's original flags, paths and Ninja cache.
export function buildGraphicsLibrary({root,build,options,frameworkGles,xr,object,compile}){
  const native=path.join(options.output,'native');
  const frameworks=names=>names.flatMap(name=>['-framework',name]);
  const webkitCallbacks=prepareWebkitCallbacks(path.join(build,'webkit-callbacks'));
  const graphicsObjects=['application-sleep','webkit-platform','webkit-binding','presentation','pixel-copy','native-surface','native-egl','composition-target','native-location','surface-texture','video-surface','video-pixels','hardware-buffer','webview','xr-presentation','overlay','display-snapshot','window-session','clock/display-clock'].map(name=>object(name,path.join(root,name+'.mm'),['-std=c++20','-fobjc-arc','-Wno-deprecated-declarations','-F'+path.dirname(options.sdl),'-DKL_NATIVE_EGL_HOST=1']));
  graphicsObjects.push(object('gles-guest-abi',prepareGlesGuestAbi(options['gles-headers'],build),['-std=c++20','-I'+path.join(options['gles-headers'],'include')]));
  graphicsObjects.push(object('webkit-jni',path.join(root,'webkit-jni.cpp'),['-std=c++20','-I'+webkitCallbacks]));
  for(const name of ['provider','compositor'])graphicsObjects.push(object('xr-'+name,xr[name],['-fobjc-arc','-fblocks','-I'+root,'-I'+path.join(options.klepton,'tests')],'clang'));
  graphicsObjects.push(object('xr-input',path.join(root,'xr-input.cpp'),['-std=c++20','-I'+build,'-F'+path.dirname(options.sdl)]));
  for(const input of [...frameworkGles,...['nio.cpp','dispatch.cpp'].map(file=>path.join(root,'framework-gles',file))])graphicsObjects.push(object('framework-gles-'+path.basename(input),input,['-std=c++20','-Wno-writable-strings','-Wno-deprecated-declarations','-I'+path.join(root,'framework-gles/include'),'-I'+path.join(build,'framework-gles'),'-I'+path.join(options['gles-headers'],'include')]));
  return compile('libklepton-graphics.dylib',['native-host.c','native-gamepad.c','gles-next.c','gles-common.c','native-heap.c','eglbridge.c','window.c','session-core.c','display-adapter.c','scheduler.c'].map(file=>path.join(root,file)).concat(graphicsObjects),
    ['-I'+build,'-DKLJ_NATIVE_OVERLAY=1','-I'+path.join(options['gles-headers'],'include'),'-F'+path.dirname(options.sdl),'-L'+native,'-lklepton-jvm','-lnativebinding','-Wl,-rpath,@loader_path','-lc++','-lz',...frameworks(['SDL3','CoreImage','Metal','QuartzCore','Foundation','AppKit','WebKit','UniformTypeIdentifiers','IOSurface','ApplicationServices','CoreVideo','Carbon'])]);
}
