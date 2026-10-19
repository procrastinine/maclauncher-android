#ifndef KLJ_DIAGNOSTICS_SUPPORT_H
#define KLJ_DIAGNOSTICS_SUPPORT_H
#include <string.h>
#include <stdio.h>
static int klj_next_debug_core(const char *version){
    int major=0,minor=0;
    return version&&sscanf(version,"OpenGL ES %d.%d",&major,&minor)==2&&(major>3||(major==3&&minor>=2));
}
static int klj_next_debug_supported(const char *version,const char *extensions){
    if(klj_next_debug_core(version))return 1;
    if(!extensions)return 0;
    const char *token="GL_KHR_debug",*at=extensions;size_t count=strlen(token);
    while((at=strstr(at,token))){if((at==extensions||at[-1]==' ')&&(at[count]==' '||at[count]=='\0'))return 1;at+=count;}
    return 0;
}
#endif
