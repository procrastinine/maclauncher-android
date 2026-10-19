#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <malloc/malloc.h>
#include "allocator-abi.h"

extern FILE *kl_host_file(void *);

static struct klj_mallinfo heap_info(void) {
    malloc_statistics_t statistics={0};
    malloc_zone_statistics(NULL,&statistics);
    // Darwin obtains its heap from VM mappings, rather than a Unix sbrk arena.
    // Its public statistics do not expose the number of free chunks.
    size_t free_bytes=statistics.size_allocated>statistics.size_in_use
        ?statistics.size_allocated-statistics.size_in_use:0;
    return (struct klj_mallinfo){.hblkhd=statistics.size_allocated,
        .usmblks=statistics.max_size_in_use,.uordblks=statistics.size_in_use,
        .fordblks=free_bytes,.keepcost=free_bytes};
}
static int heap_xml(int options,void *stream) {
    if(options){errno=EINVAL;return -1;}
    FILE *file=kl_host_file(stream);
    if(!file){errno=EBADF;return -1;}
    struct klj_mallinfo information=heap_info();
    // malloc_info XML is allocator-versioned on Android too. Report only
    // statistics actually supplied by the allocator, without fabricated bins.
    return fprintf(file,"<malloc version=\"darwin-1\"><heap nr=\"0\"><allocated>%zu</allocated><free>%zu</free><reserved>%zu</reserved></heap></malloc>\n",
        information.uordblks,information.fordblks,information.hblkhd)<0?-1:0;
}
void *klj_allocator_lookup(const char *name) {
    if(!strcmp(name,"mallinfo")||!strcmp(name,"mallinfo2"))return (void *)heap_info;
    if(!strcmp(name,"malloc_info"))return (void *)heap_xml;
    return NULL;
}
