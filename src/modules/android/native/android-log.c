#include "android-log.h"
#include "process-ids.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

static pthread_once_t once=PTHREAD_ONCE_INIT;
static int destination=-1;
static void initialize(void){destination=klj_process_descriptor_duplicate(STDERR_FILENO);}
void klj_android_log_initialize(void){int saved=errno;pthread_once(&once,initialize);errno=saved;}
int klj_android_log_write(int priority,const char *tag,const char *message){
    int saved=errno;klj_android_log_initialize();
    if(destination<0){errno=saved;return -1;}
    if(!tag)tag="";if(!message)message="";
    char prefix[32];int length=snprintf(prefix,sizeof prefix,"[%d/",priority);
    struct iovec vectors[]={{prefix,(size_t)length},{(void *)tag,strlen(tag)},
        {"] ",2},{(void *)message,strlen(message)},{"\n",1}};
    size_t first=0,total=0;
    while(first<sizeof vectors/sizeof *vectors){
        ssize_t written=writev(destination,vectors+first,(int)(sizeof vectors/sizeof *vectors-first));
        if(written<0){if(errno==EINTR)continue;errno=saved;return -1;}
        if(!written){errno=saved;return -1;}total+=(size_t)written;
        size_t bytes=(size_t)written;
        while(first<sizeof vectors/sizeof *vectors&&bytes>=vectors[first].iov_len){bytes-=vectors[first].iov_len;first++;}
        if(first<sizeof vectors/sizeof *vectors){vectors[first].iov_base=(char *)vectors[first].iov_base+bytes;vectors[first].iov_len-=bytes;}
    }
    errno=saved;return total>0x7fffffff?0x7fffffff:(int)total;
}
