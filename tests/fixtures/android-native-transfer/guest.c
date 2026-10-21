#include <errno.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/types.h>
int guest_advice(void *address,size_t length,int advice){return posix_madvise(address,length,advice);}
ssize_t guest_sendfile(int output,int input,off_t *offset,size_t count){return sendfile(output,input,offset,count);}
ssize_t guest_sendfile64(int output,int input,off64_t *offset,size_t count){return sendfile64(output,input,offset,count);}
int guest_errno(void){return errno;}
