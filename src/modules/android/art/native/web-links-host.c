#include "web-links-bridge.h"
#include "web-links-platform.h"
#include <stddef.h>
int64_t maclauncher_art_web_links(uint64_t address,uint64_t size){
    if(!address||size!=sizeof(struct KljArtWebLinks))return -22;
    const struct KljArtWebLinks*p=(const void*)(uintptr_t)address;
    switch(p->operation){case 0:return klj_web_links_configure(p->allowed);case 1:return klj_web_links_open((const void*)(uintptr_t)p->bytes,p->length);case 2:klj_web_links_close();return 0;default:return -22;}
}
