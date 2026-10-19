/* Android's original termios implementation calls this physical boundary.
 * Darwin owns the actual descriptor and terminal; Android owns its records. */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "android-terminal.h"

struct Flag { uint32_t android; tcflag_t host; };
struct Group { uint32_t android; tcflag_t host; const struct Flag *values; size_t count; };
#define COUNT(array) (sizeof(array)/sizeof((array)[0]))
#include "terminal-maps.h"

static tcflag_t flagsToHost(uint32_t input,const struct Flag *flags,size_t count){
    tcflag_t result=0;
    for(size_t i=0;i<count;i++)if((input&flags[i].android)==flags[i].android)result|=flags[i].host;
    return result;
}
static uint32_t flagsToAndroid(tcflag_t input,const struct Flag *flags,size_t count){
    uint32_t result=0;
    for(size_t i=0;i<count;i++)if((input&flags[i].host)==flags[i].host)result|=flags[i].android;
    return result;
}
static tcflag_t groupToHost(uint32_t input,const struct Group *group){
    input&=group->android;
    for(size_t i=0;i<group->count;i++)if(input==group->values[i].android)return group->values[i].host;
    return 0;
}
static uint32_t groupToAndroid(tcflag_t input,const struct Group *group){
    input&=group->host;
    for(size_t i=0;i<group->count;i++)if(input==group->values[i].host)return group->values[i].android;
    return 0;
}
static int toHost(const struct klj_android_termios *android,struct termios *host){
    memset(host,0,sizeof(*host));
    host->c_iflag=flagsToHost(android->c_iflag,iflagFlags,COUNT(iflagFlags));
    host->c_oflag=flagsToHost(android->c_oflag,oflagFlags,COUNT(oflagFlags));
    host->c_cflag=flagsToHost(android->c_cflag,cflagFlags,COUNT(cflagFlags))|groupToHost(android->c_cflag,&sizeGroup);
    host->c_lflag=flagsToHost(android->c_lflag,lflagFlags,COUNT(lflagFlags));
    for(size_t i=0;i<COUNT(outputGroups);i++)host->c_oflag|=groupToHost(android->c_oflag,&outputGroups[i]);
    memset(host->c_cc,_POSIX_VDISABLE,sizeof(host->c_cc));
    for(size_t i=0;i<COUNT(controls);i++){
        cc_t value=android->c_cc[controls[i].android];
        if(controls[i].android!=KLJ_ANDROID_VMIN&&controls[i].android!=KLJ_ANDROID_VTIME&&value==0)value=_POSIX_VDISABLE;
        host->c_cc[controls[i].host]=value;
    }
    uint32_t speed=android->c_cflag&KLJ_ANDROID_CBAUD;
    for(size_t i=0;i<COUNT(speeds);i++)if(speed==speeds[i].android)return cfsetspeed(host,speeds[i].host);
    errno=EINVAL;return -1;
}
static void toAndroid(const struct termios *host,struct klj_android_termios *android){
    memset(android,0,sizeof(*android));
    android->c_iflag=flagsToAndroid(host->c_iflag,iflagFlags,COUNT(iflagFlags));
    android->c_oflag=flagsToAndroid(host->c_oflag,oflagFlags,COUNT(oflagFlags));
    android->c_cflag=flagsToAndroid(host->c_cflag,cflagFlags,COUNT(cflagFlags))|groupToAndroid(host->c_cflag,&sizeGroup);
    android->c_lflag=flagsToAndroid(host->c_lflag,lflagFlags,COUNT(lflagFlags));
    for(size_t i=0;i<COUNT(outputGroups);i++)android->c_oflag|=groupToAndroid(host->c_oflag,&outputGroups[i]);
    for(size_t i=0;i<COUNT(controls);i++){
        cc_t value=host->c_cc[controls[i].host];
        if(controls[i].android!=KLJ_ANDROID_VMIN&&controls[i].android!=KLJ_ANDROID_VTIME&&value==_POSIX_VDISABLE)value=0;
        android->c_cc[controls[i].android]=value;
    }
    speed_t speed=cfgetospeed(host);
    for(size_t i=0;i<COUNT(speeds);i++)if(speed==speeds[i].host){android->c_cflag|=speeds[i].android;break;}
}
static int getAttributes(int fd,void *record){
    if(!record){errno=EFAULT;return -1;}
    struct termios host;if(tcgetattr(fd,&host))return -1;
    struct klj_android_termios android;toAndroid(&host,&android);memcpy(record,&android,sizeof(android));return 0;
}
static int setAttributes(int fd,int action,const void *record){
    if(!record){errno=EFAULT;return -1;}
    struct klj_android_termios android;memcpy(&android,record,sizeof(android));
    struct termios host;if(toHost(&android,&host))return -1;return tcsetattr(fd,action,&host);
}

/* Returning zero means this request belongs to the existing nonterminal ioctl
 * owner. It is not a failure and does not prevent any other descriptor path. */
int klj_terminal_ioctl_dispatch(int fd,unsigned long request,uintptr_t argument,int *result){
    switch(request){
    case KLJ_ANDROID_TCGETS:*result=getAttributes(fd,(void *)argument);return 1;
    case KLJ_ANDROID_TCSETS:*result=setAttributes(fd,TCSANOW,(const void *)argument);return 1;
    case KLJ_ANDROID_TCSETSW:*result=setAttributes(fd,TCSADRAIN,(const void *)argument);return 1;
    case KLJ_ANDROID_TCSETSF:*result=setAttributes(fd,TCSAFLUSH,(const void *)argument);return 1;
    case KLJ_ANDROID_TCSBRK:*result=argument?tcdrain(fd):tcsendbreak(fd,0);return 1;
    case KLJ_ANDROID_TCSBRKP:*result=tcsendbreak(fd,(int)argument);return 1;
    case KLJ_ANDROID_TCXONC:{
        int action;
        switch(argument){
        case KLJ_ANDROID_TCOOFF:action=TCOOFF;break;
        case KLJ_ANDROID_TCOON:action=TCOON;break;
        case KLJ_ANDROID_TCIOFF:action=TCIOFF;break;
        case KLJ_ANDROID_TCION:action=TCION;break;
        default:errno=EINVAL;*result=-1;return 1;
        }
        *result=tcflow(fd,action);return 1;
    }
    case KLJ_ANDROID_TCFLSH:{
        int queue;
        switch(argument){
        case KLJ_ANDROID_TCIFLUSH:queue=TCIFLUSH;break;
        case KLJ_ANDROID_TCOFLUSH:queue=TCOFLUSH;break;
        case KLJ_ANDROID_TCIOFLUSH:queue=TCIOFLUSH;break;
        default:errno=EINVAL;*result=-1;return 1;
        }
        *result=tcflush(fd,queue);return 1;
    }
    case KLJ_ANDROID_TIOCGWINSZ:{
        struct winsize host;*result=ioctl(fd,TIOCGWINSZ,&host);
        if(!*result){struct klj_android_winsize android={host.ws_row,host.ws_col,host.ws_xpixel,host.ws_ypixel};memcpy((void *)argument,&android,sizeof(android));}
        return 1;
    }
    case KLJ_ANDROID_TIOCSWINSZ:{
        struct klj_android_winsize android;memcpy(&android,(const void *)argument,sizeof(android));
        struct winsize host={android.ws_row,android.ws_col,android.ws_xpixel,android.ws_ypixel};*result=ioctl(fd,TIOCSWINSZ,&host);return 1;
    }
    case KLJ_ANDROID_TIOCGPGRP:*result=ioctl(fd,TIOCGPGRP,(void *)argument);return 1;
    case KLJ_ANDROID_TIOCSPGRP:*result=ioctl(fd,TIOCSPGRP,(void *)argument);return 1;
    case KLJ_ANDROID_TIOCGSID:{pid_t sid=tcgetsid(fd);*result=sid<0?-1:0;if(sid>=0)memcpy((void *)argument,&sid,sizeof(sid));return 1;}
    case KLJ_ANDROID_TIOCEXCL:*result=ioctl(fd,TIOCEXCL);return 1;
    case KLJ_ANDROID_TIOCNXCL:*result=ioctl(fd,TIOCNXCL);return 1;
    case KLJ_ANDROID_TIOCNOTTY:*result=ioctl(fd,TIOCNOTTY);return 1;
    case KLJ_ANDROID_TIOCSBRK:*result=ioctl(fd,TIOCSBRK);return 1;
    case KLJ_ANDROID_TIOCCBRK:*result=ioctl(fd,TIOCCBRK);return 1;
    case KLJ_ANDROID_TIOCOUTQ:*result=ioctl(fd,TIOCOUTQ,(void *)argument);return 1;
    default:return 0;
    }
}

extern int kl_ioctl(int,unsigned long,uintptr_t);
int klj_terminal_ioctl(int fd,int request,uintptr_t argument){return kl_ioctl(fd,(unsigned int)request,argument);}
