#include <jni.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <string.h>

#include "publisher-bindings.h"
#include "../native-import-registry.h"

// Every API in the original compiled NDK module is available to the ELF loader.
// The core registry owns lookup ordering and idempotent installation.
static void* resolveNdk(const char* name) {
    for (const auto& entry : ndkExports) {
        if (!strcmp(entry.name,name)) return entry.address;
    }
    return nullptr;
}

static bool publisherReady;
static void __attribute__((constructor(101))) bindPublisher() {
    Dl_info own{};
    if (!dladdr(reinterpret_cast<void*>(&bindPublisher),&own)) return;
    std::string directory(own.dli_fname);
    directory.resize(directory.find_last_of('/')+1);
    void* publisher=dlopen((directory+"libandroid_runtime.dylib").c_str(),RTLD_LAZY|RTLD_LOCAL);
    Dl_info image{};
    if (!publisher||!dladdr(dlsym(publisher,"JNI_OnLoad"),&image)) return;
    for (size_t i=0;i<sizeof(publisherOffsets)/sizeof(*publisherOffsets);i++) {
        void* shared=dlsym(RTLD_DEFAULT,publisherNames[i]);
        publisherTargets[i]=shared ? reinterpret_cast<uintptr_t>(shared) : reinterpret_cast<uintptr_t>(image.dli_fbase)+publisherOffsets[i];
    }
    publisherReady=true;
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*, void*) {
    if (!publisherReady) return JNI_ERR;
    Dl_info own{};if (!dladdr(reinterpret_cast<void*>(&JNI_OnLoad),&own)) return JNI_ERR;
    std::string directory(own.dli_fname);directory.resize(directory.find_last_of('/')+1);
    void* core=dlopen((directory+"libklepton-jvm.dylib").c_str(),RTLD_NOW|RTLD_LOCAL);
    auto install=core ? reinterpret_cast<int (*)(KljImportResolver)>(dlsym(core,"klj_register_import_resolver")) : nullptr;
    if (!install||!install(resolveNdk)) return JNI_ERR;
    return JNI_VERSION_1_6;
}
