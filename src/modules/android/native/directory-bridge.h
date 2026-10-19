#pragma once
#include <stdint.h>

typedef struct {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
} klj_dirent;

/* The opaque guest stream owns its translated result and exactly one upstream
 * real or OBB stream. No process-wide guest handle/result cache is needed. */
void *klj_directory_create(void *stream, int virtual_stream);
int klj_directory_close(void *stream);
klj_dirent *klj_directory_read(void *stream);
int klj_directory_read_r(void *stream, klj_dirent *entry, klj_dirent **result);
void *klj_directory_fdopen(int fd);
int klj_directory_descriptor(void *stream);
void klj_directory_rewind(void *stream);
long klj_directory_tell(void *stream);
void klj_directory_seek(void *stream, long position);
void *klj_directory_lookup(const char *name);
