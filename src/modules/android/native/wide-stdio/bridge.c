#include "bridge.h"
#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "kl_va.h"

extern FILE *kl_host_file(void *);
extern int kl_errno_to_linux(int), kl_errno_from_linux(int);
extern int klj_wide_format(KLJWideOutput *,const wchar_t *,kl_va *);
static _Thread_local int *format_error;
static _Thread_local int fallback_error;
int *klj_wide_errno(void){return format_error?format_error:&fallback_error;}
static void record_error(void){*klj_wide_errno()=kl_errno_to_linux(errno);}
char *klj_wide_radix(void){return nl_langinfo(RADIXCHAR);}
char *klj_wide_strerror(int value,char *buffer,size_t size){
    if(strerror_r(kl_errno_from_linux(value),buffer,size))snprintf(buffer,size,"Unknown error %d",value);
    return buffer;
}
void *klj_wide_mmap(void *address,size_t size,int protection,int flags,int fd,int64_t offset){
    int native=(flags&1?MAP_SHARED:0)|(flags&2?MAP_PRIVATE:0)|(flags&0x10?MAP_FIXED:0)|(flags&0x20?MAP_ANON:0);
    void *result=mmap(address,size,protection,native,fd,offset);if(result==MAP_FAILED)record_error();return result;
}
void klj_wide_fatal(const char *message,...){
    va_list arguments;va_start(arguments,message);vfprintf(stderr,message,arguments);va_end(arguments);fputc('\n',stderr);abort();
}
int klj_wide_can_write(KLJWideOutput *out){
    if(!out->stream)return 0;
    FILE *stream=out->stream;
    // BSD's FILE owns access mode, including streams without a descriptor.
    return !(stream->_flags&(__SWR|__SRW));
}
wint_t klj_wide_put(wchar_t character,KLJWideOutput *out){
    if(out->stream){wint_t result=fputwc(character,out->stream);if(result==WEOF){out->error=1;record_error();}return result;}
    // Original Bionic string formatting validates each wide character through
    // the active locale before converting the temporary byte stream back.
    // Preserve that validation while writing directly into the caller's buffer.
    char bytes[MB_LEN_MAX];mbstate_t state={0};
    if(wcrtomb(bytes,character,&state)==(size_t)-1){out->error=1;record_error();return WEOF;}
    if(out->written+1<out->capacity)out->buffer[out->written]=character;
    out->written++;return (wint_t)character;
}
static int format(KLJWideOutput *out,const wchar_t *text,kl_va *arguments){
    int error=kl_errno_to_linux(errno),*previous=format_error;format_error=&error;
    kl_va copied=*arguments;
    int result=klj_wide_format(out,text,&copied);
    format_error=previous;errno=kl_errno_from_linux(error);return result;
}
int klj_wide_vfwprintf(void *guest,const wchar_t *text,kl_va *arguments){
    FILE *stream=kl_host_file(guest);if(!stream){errno=EBADF;return -1;}
    flockfile(stream);fwide(stream,1);
    KLJWideOutput out={.stream=stream};int result=format(&out,text,arguments);
    funlockfile(stream);return result;
}
int klj_wide_vwprintf(const wchar_t *text,kl_va *arguments){return klj_wide_vfwprintf(stdout,text,arguments);}
int klj_wide_vswprintf(wchar_t *buffer,size_t capacity,const wchar_t *text,kl_va *arguments){
    if(!capacity){errno=EINVAL;return -1;}
    KLJWideOutput out={.buffer=buffer,.capacity=capacity};int result=format(&out,text,arguments);
    buffer[out.written<capacity?out.written:capacity-1]=0;
    if(result>=0&&out.written>=capacity){errno=EOVERFLOW;return -1;}
    return result;
}
