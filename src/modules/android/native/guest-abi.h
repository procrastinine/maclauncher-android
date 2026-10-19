#pragma once
#include <stdlib.h>
#include <string.h>
/* Set by the launcher from the selected APK ABI before the JVM starts. The
 * executing host remains ARM64; this describes only guest native pointers. */
static const char *klj_guest_abi(void){
    const char *value=getenv("MACLAUNCHER_ANDROID_NATIVE_ABI");
    return value&&(!strcmp(value,"armeabi-v7a")||!strcmp(value,"armeabi"))?value:"arm64-v8a";
}
static int klj_guest_is64(void){return !strcmp(klj_guest_abi(),"arm64-v8a");}
