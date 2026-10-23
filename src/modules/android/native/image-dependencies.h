#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Retain the original DT_NEEDED order before constructors can replace the
 * mapped dynamic/string tables. The existing image owns these names. */
typedef struct {char **names;size_t count;uint64_t flags_1;} KlImageDependencies;
static void klj_image_dependencies_release(KlImageDependencies *value){
    for(size_t i=0;i<value->count;i++)free(value->names[i]);
    free(value->names);*value=(KlImageDependencies){0};
}
static int klj_image_dependencies_capture(KlImageDependencies *out,const void *base,size_t span,
        const uint64_t *dynamic,size_t bytes,const char *strings){
    uintptr_t first=(uintptr_t)base,at=(uintptr_t)dynamic,text=(uintptr_t)strings;
    if(at<first||at-first>span||bytes>span-(at-first)||text<first||text-first>span)return 0;
    size_t count=0,string_bytes=0,entries=bytes/(2*sizeof(uint64_t)),end=0;
    for(;end<entries;end++){
        uint64_t tag=dynamic[2*end],value=dynamic[2*end+1];
        if(!tag)break;
        if(tag==1)count++; /* DT_NEEDED */
        if(tag==10){if(value>SIZE_MAX)return 0;string_bytes=(size_t)value;} /* DT_STRSZ */
    }
    if(end==entries||string_bytes>span-(text-first)||count>SIZE_MAX/sizeof(char *))return 0;
    KlImageDependencies next={0};
    for(size_t i=0;i<end;i++)if(dynamic[2*i]==0x6ffffffb)next.flags_1=dynamic[2*i+1]; /* DT_FLAGS_1 */
    if(count&&!(next.names=calloc(count,sizeof(char *))))return 0;
    for(size_t i=0;i<end;i++)if(dynamic[2*i]==1){
        uint64_t offset=dynamic[2*i+1];
        if(offset>=string_bytes)goto failed;
        const char *name=strings+offset,*stop=memchr(name,0,string_bytes-(size_t)offset);
        if(!stop||stop==name)goto failed;
        size_t length=(size_t)(stop-name)+1;
        char *copy=malloc(length);if(!copy)goto failed;
        memcpy(copy,name,length);next.names[next.count++]=copy;
    }
    klj_image_dependencies_release(out);*out=next;return 1;
failed:
    klj_image_dependencies_release(&next);return 0;
}
