#include "web-links-platform.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// One inherited, nonblocking request capability to the existing launcher.
// The launcher independently validates URLs and owns browser consent.
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int configured,descriptor=-1;
int klj_web_links_configure(int allowed){
    int failed=0;pthread_mutex_lock(&lock);
    if(configured)failed=1;
    else{configured=1;if(allowed){struct stat info;int flags=fcntl(3,F_GETFL);
        if(flags<0||fstat(3,&info)!=0||(!S_ISFIFO(info.st_mode)&&!S_ISSOCK(info.st_mode))||
            fcntl(3,F_SETFL,flags|O_NONBLOCK)<0||fcntl(3,F_SETFD,FD_CLOEXEC)<0||fcntl(3,F_SETNOSIGPIPE,1)<0)failed=2;
        else descriptor=3;}}
    int available=descriptor>=0;pthread_mutex_unlock(&lock);
    if(failed==2)fprintf(stderr,"[android-host] Web-link request channel is unavailable\n");
    return failed==1?-1:!failed&&available;
}
int klj_web_links_open(const void*url,size_t length){
    if(!url||!length||length>8192)return 0;
    unsigned char frame[8194];frame[0]=length>>8;frame[1]=length&255;memcpy(frame+2,url,length);
    pthread_mutex_lock(&lock);size_t sent=0,total=length+2;
    for(unsigned attempt=0;descriptor>=0&&sent<total&&attempt<16;attempt++){
        ssize_t count=write(descriptor,frame+sent,total-sent);
        if(count>0)sent+=(size_t)count;else if(count<0&&errno==EINTR)continue;else break;
    }
    if(sent!=total&&descriptor>=0){close(descriptor);descriptor=-1;}
    pthread_mutex_unlock(&lock);return sent==total;
}
void klj_web_links_close(void){pthread_mutex_lock(&lock);if(descriptor>=0){close(descriptor);descriptor=-1;}configured=1;pthread_mutex_unlock(&lock);}
