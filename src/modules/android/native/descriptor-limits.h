#pragma once
#include <stdint.h>
#include <errno.h>
#include <sys/sysctl.h>

// Darwin permits an unlimited rlimit even though open() still enforces this
// finite per-process kernel ceiling. Linux's NOFILE hard limit cannot exceed
// its corresponding nr_open ceiling. Preserve lower process-specific limits.
static int klj_descriptor_ceiling(uint64_t *maximum){
 int value=0;size_t size=sizeof(value);
 if(sysctlbyname("kern.maxfilesperproc",&value,&size,NULL,0))return -1;
 if(size!=sizeof(value)||value<=0){errno=EIO;return -1;}
 *maximum=(uint64_t)value;return 0;
}
static int klj_descriptor_limits(uint64_t *current,uint64_t *maximum){
 uint64_t ceiling;if(klj_descriptor_ceiling(&ceiling))return -1;
 if(*maximum>ceiling)*maximum=ceiling;
 if(*current>*maximum)*current=*maximum;
 return 0;
}
