#ifndef MACLAUNCHER_NATIVE_ZLIB_IMPORTS_H
#define MACLAUNCHER_NATIVE_ZLIB_IMPORTS_H
#include <dlfcn.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <zlib.h>

/* zlib's public LP64 buffer/stream ABI is the same on Android arm64 and macOS.
 * Fixed-argument allocator and inflateBack callbacks also share that ABI.
 * The original gzip file family below shares the guest descriptor owner.
 * Never resolve unrelated names from the process namespace. */
extern void *klj_gzip_lookup(const char *);
_Static_assert(sizeof(uInt)==4&&sizeof(uLong)==8&&sizeof(z_off_t)==8,"zlib LP64 scalar ABI");
_Static_assert(sizeof(z_stream)==112&&offsetof(z_stream,total_in)==16&&offsetof(z_stream,zalloc)==64&&offsetof(z_stream,adler)==96,"zlib LP64 stream ABI");
_Static_assert(sizeof(gz_header)==80&&offsetof(gz_header,time)==8&&offsetof(gz_header,comment)==56,"zlib LP64 header ABI");
static struct { const char *name;void *function; } klj_zlib_exports[]={
    {"zlibVersion",NULL},{"zlibCompileFlags",NULL},{"zError",NULL},{"get_crc_table",NULL},
    {"compress",NULL},{"compress2",NULL},{"compressBound",NULL},{"uncompress",NULL},{"uncompress2",NULL},
    {"deflateInit_",NULL},{"deflateInit2_",NULL},{"deflate",NULL},{"deflateEnd",NULL},
    {"deflateSetDictionary",NULL},{"deflateGetDictionary",NULL},{"deflateCopy",NULL},
    {"deflateReset",NULL},{"deflateParams",NULL},{"deflateTune",NULL},{"deflateBound",NULL},
    {"deflatePending",NULL},{"deflatePrime",NULL},{"deflateSetHeader",NULL},{"deflateResetKeep",NULL},
    {"inflateInit_",NULL},{"inflateInit2_",NULL},{"inflate",NULL},{"inflateEnd",NULL},
    {"inflateSetDictionary",NULL},{"inflateGetDictionary",NULL},{"inflateSync",NULL},{"inflateCopy",NULL},
    {"inflateReset",NULL},{"inflateReset2",NULL},{"inflatePrime",NULL},{"inflateMark",NULL},{"inflateGetHeader",NULL},
    {"inflateBackInit_",NULL},{"inflateBack",NULL},{"inflateBackEnd",NULL},
    {"inflateSyncPoint",NULL},{"inflateUndermine",NULL},{"inflateValidate",NULL},{"inflateCodesUsed",NULL},{"inflateResetKeep",NULL},
    {"adler32",NULL},{"adler32_z",NULL},{"adler32_combine",NULL},{"adler32_combine64",NULL},
    {"crc32",NULL},{"crc32_z",NULL},{"crc32_combine",NULL},{"crc32_combine64",NULL},
    {"crc32_combine_gen",NULL},{"crc32_combine_gen64",NULL},{"crc32_combine_op",NULL}
};
static pthread_once_t klj_zlib_once=PTHREAD_ONCE_INIT;
// AOSP z_crc_t is uint32_t; Darwin keeps unsigned long for this legacy export.
// Convert its immutable 256-entry public table once, not compression state.
static uint32_t klj_zlib_crc_entries[256];
static const uint32_t *klj_zlib_crc_table(void){return klj_zlib_crc_entries;}
static void klj_zlib_initialize(void) {
    /* Keep the existing system image alive while guest function pointers exist. */
    void *library=dlopen("/usr/lib/libz.1.dylib",RTLD_LAZY|RTLD_LOCAL);
    if(!library)return;
    for(size_t i=0;i<sizeof klj_zlib_exports/sizeof klj_zlib_exports[0];i++){
        const char *name=klj_zlib_exports[i].name;
        void *function=dlsym(library,name);
        // Android exposes both spellings; Darwin's unsuffixed offsets are 64 bit.
        if(!function&&!strcmp(name,"adler32_combine64"))function=dlsym(library,"adler32_combine");
        if(!function&&!strcmp(name,"crc32_combine64"))function=dlsym(library,"crc32_combine");
        if(!function&&!strcmp(name,"crc32_combine_gen64"))function=dlsym(library,"crc32_combine_gen");
        if(function&&!strcmp(name,"get_crc_table")){
            const z_crc_t *host=((const z_crc_t *(*)(void))function)();
            for(size_t n=0;n<256;n++)klj_zlib_crc_entries[n]=(uint32_t)host[n];
            function=(void *)klj_zlib_crc_table;
        }
        klj_zlib_exports[i].function=function;
    }
}
static void *klj_zlib_lookup(const char *name) {
    void *gzip=klj_gzip_lookup(name);if(gzip)return gzip;
    for(size_t i=0;i<sizeof klj_zlib_exports/sizeof klj_zlib_exports[0];i++)if(!strcmp(name,klj_zlib_exports[i].name)){
        pthread_once(&klj_zlib_once,klj_zlib_initialize);
        return klj_zlib_exports[i].function;
    }
    return NULL;
}
#endif
