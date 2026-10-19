#pragma once
#include <stdint.h>

/* The VM supplies its own native memory accounting. ActivityManager applies
 * the same low-memory policy without inventing another memory budget. */
static inline void klj_memory_info_values(uint64_t total,uint64_t available,int percent,int64_t values[4]) {
    uint64_t threshold=percent<=0?0:total/100*(uint64_t)(100-(percent>100?100:percent));
    values[0]=(int64_t)available;values[1]=(int64_t)total;
    values[2]=(int64_t)threshold;values[3]=threshold&&available<threshold;
}
