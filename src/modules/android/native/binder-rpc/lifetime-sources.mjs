import fs from 'node:fs';
import path from 'node:path';

function replace(text,before,after){if(text.split(before).length!==2)throw Error('AOSP Binder lifetime boundary changed: '+before);return text.replace(before,after);}

/** Native process caches belong to the embedding VM, not C++ exit order.
 * Keep the original references and destructors; release their roots while JNI
 * is available after the component has drained its transport producers. */
export function prepareBinderLifetime({sources,binder,output}){
  for(const file of sources){
    // Generated AIDL units can share the original interface basenames.
    // Lifetime roots belong to these complete native translation units only.
    if(![path.join(binder,'IServiceManager.cpp'),path.join(binder,'IMemory.cpp'),
        path.join(output,'ndk/BackendUnifiedServiceManager-darwin.cpp')].includes(file))continue;
    let text=fs.readFileSync(file,'utf8');
    if(file===path.join(binder,'IServiceManager.cpp')){
      const locals='    static std::mutex gPermissionControllerLock;\n    static sp<IPermissionController> gPermissionController;\n';
      text=replace(text,locals,'');
      text=replace(text,'bool checkPermission(const String16& permission, pid_t pid, uid_t uid, bool logPermissionFailure) {',
        'static std::mutex gPermissionControllerLock;\nstatic sp<IPermissionController> gPermissionController;\n\nbool checkPermission(const String16& permission, pid_t pid, uid_t uid, bool logPermissionFailure) {');
      text+='\nnamespace android {\nvoid kljRetireServiceManager(){\n    sp<IPermissionController> permission;\n    {std::lock_guard lock(gPermissionControllerLock);permission=std::move(gPermissionController);}\n    permission.clear();\n    gDefaultServiceManager.clear();\n    std::vector<AccessorProviderEntry> providers;\n    {std::lock_guard lock(gAccessorProvidersMutex);providers.swap(gAccessorProviders);}\n}\n}\n';
    }else if(file===path.join(output,'ndk/BackendUnifiedServiceManager-darwin.cpp')){
      text+='\nnamespace android {void kljRetireUnifiedServiceManager(){gUnifiedServiceManager.clear();}}\n';
    }else if(file===path.join(binder,'IMemory.cpp')){
      text+='\nnamespace android {void kljRetireBinderMemory(){gHeapCache.clear();}}\n';
    }else continue;
    fs.writeFileSync(file,text);
  }
}
