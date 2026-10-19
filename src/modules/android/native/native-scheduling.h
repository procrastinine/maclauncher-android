#ifndef MACLAUNCHER_NATIVE_SCHEDULING_H
#define MACLAUNCHER_NATIVE_SCHEDULING_H
#include <pthread.h>
#include <errno.h>
#include <stddef.h>

int klj_getcpu(unsigned *cpu,unsigned *node,void *cache){
    (void)cache;size_t current=0;int error=pthread_cpu_number_np(&current);
    if(error){errno=error;return -1;}
    if(cpu)*cpu=(unsigned)current;
    if(node)*node=0; // Apple Silicon exposes one UMA memory node.
    return 0;
}
static int klj_sched_getcpu(void){unsigned cpu;return klj_getcpu(&cpu,NULL,NULL)<0?-1:(int)cpu;}
#endif
