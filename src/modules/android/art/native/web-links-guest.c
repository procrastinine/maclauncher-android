#include "bridge.h"
#include "web-links-bridge.h"
#include "web-links-platform.h"
int klj_web_links_configure(int allowed){struct KljArtWebLinks p={.operation=0,.allowed=allowed!=0};return (int)klj_art_host(10,&p,sizeof(p));}
int klj_web_links_open(const void*bytes,size_t length){struct KljArtWebLinks p={.operation=1,.bytes=(uintptr_t)bytes,.length=length};return (int)klj_art_host(10,&p,sizeof(p));}
void klj_web_links_close(void){struct KljArtWebLinks p={.operation=2};klj_art_host(10,&p,sizeof(p));}
