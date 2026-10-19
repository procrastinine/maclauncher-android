#pragma once
#include <unistd.h>

/* Bionic bits/seek_constants.h defines DATA=3 and HOLE=4. Darwin reverses
 * those two values. Translate at the syscall boundary; file/OBB ownership,
 * offsets and native errno remain with their existing implementation. */
static inline int klj_seek_whence_to_host(int value) {
    switch (value) {
        case 0: return SEEK_SET;
        case 1: return SEEK_CUR;
        case 2: return SEEK_END;
        case 3: return SEEK_DATA;
        case 4: return SEEK_HOLE;
        default: return value;
    }
}
