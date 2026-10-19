#ifndef MACLAUNCHER_ANDROID_PROPERTIES_H
#define MACLAUNCHER_ANDROID_PROPERTIES_H
#include <stddef.h>
#include "guest-abi.h"
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The hosted Android API's identity. No serial, certification or physical
 * Android hardware is inferred from the Mac. Unrelated properties retain the
 * existing runtime provider. Strings/opaque handles have process lifetime. */
static const struct klj_android_property { const char *name, *value; } klj_android_properties[] = {
    {"ro.product.manufacturer","Android"}, {"ro.product.brand","Android"},
    {"ro.product.model","Android"}, {"ro.product.device","Android"},
    {"ro.product.name","Android"}, {"ro.hardware","arm64"},
    {"ro.product.board","unknown"}, {"ro.bootloader","unknown"},
    {"ro.build.id","Android"}, {"ro.build.display.id","Android"},
    {"ro.build.type","user"}, {"ro.build.tags",""},
    {"ro.build.fingerprint","Android/Android/Android:16/Android/Android:user/"},
    {"ro.build.version.release","16"}, {"ro.build.version.incremental","Android"},
    {"ro.build.version.codename","REL"}, {"ro.build.version.sdk","36"},
    {"ro.build.version.sdk_full","36.0"},
    {"ro.build.version.all_codenames","REL"},
    {"ro.vendor.build.security_patch",""},
    {"ro.build.version.preview_sdk","0"}, {"ro.build.version.security_patch",""},
    {"ro.product.cpu.abi","arm64-v8a"}, {"ro.product.cpu.abi2",""},
    {"ro.product.cpu.abilist","arm64-v8a"}, {"ro.product.cpu.abilist64","arm64-v8a"},
    {"ro.product.cpu.abilist32",""}, {"ro.serialno","unknown"}
};
static const struct klj_android_property *klj_android_property_named(const char *name) {
    static const struct klj_android_property arm32_properties[]={
      {"ro.product.cpu.abi","armeabi-v7a"},{"ro.product.cpu.abi2","armeabi"},
      {"ro.product.cpu.abilist","arm64-v8a,armeabi-v7a,armeabi"},
      {"ro.product.cpu.abilist32","armeabi-v7a,armeabi"}};
    if(name&&!klj_guest_is64())for(size_t i=0;i<sizeof arm32_properties/sizeof *arm32_properties;i++)
      if(!strcmp(name,arm32_properties[i].name))return &arm32_properties[i];
    if(name)for(size_t i=0;i<sizeof klj_android_properties/sizeof *klj_android_properties;i++)
        if(!strcmp(name,klj_android_properties[i].name))return &klj_android_properties[i];
    return NULL;
}
#include "property-metadata.h"
/* Original Bionic API queries this same process-local property owner. */
#define __BIONIC_GET_DEVICE_API_LEVEL_INLINE static inline
#define __system_property_get klj_android_property_get
#ifndef __attribute_pure__
#define __attribute_pure__ __attribute__((pure))
#endif
#include KLEPTON_DEVICE_API_SOURCE
#undef __system_property_get
#undef __BIONIC_GET_DEVICE_API_LEVEL_INLINE
static void *klj_android_property_lookup(const char *name) {
    if(!strcmp(name,"android_get_device_api_level"))return (void *)android_get_device_api_level;
    if(!strcmp(name,"__system_property_get"))return (void *)klj_android_property_get;
    if(!strcmp(name,"__system_property_find"))return (void *)klj_android_property_find;
    if(!strcmp(name,"__system_property_read"))return (void *)klj_android_property_read;
    if(!strcmp(name,"__system_property_read_callback"))return (void *)klj_android_property_read_callback;
    if(!strcmp(name,"__system_property_serial"))return (void *)klj_android_property_serial;
    if(!strcmp(name,"__system_property_foreach"))return (void *)klj_android_property_foreach;
    if(!strcmp(name,"__system_property_find_nth"))return (void *)klj_android_property_find_nth;
    if(!strcmp(name,"__system_property_set"))return (void *)klj_android_property_set;
    if(!strcmp(name,"__system_property_area_serial"))return (void *)klj_android_property_area_serial;
    if(!strcmp(name,"__system_property_wait"))return (void *)klj_android_property_wait;
    if(!strcmp(name,"__system_property_wait_any"))return (void *)klj_android_property_wait_any;
    return NULL;
}
#endif
