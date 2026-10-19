#include "operations.h"
#include "../pollable-fds/imports.h"
/* POSIX operations inside the native libc adapter keep their actual Darwin
 * implementation, adding only notifications missing from Darwin's kqueue. */
#define close klj_file_close
#define fclose klj_file_fclose
#undef read
#define read klj_file_read
#undef pread
#define pread klj_file_pread
#undef readv
#define readv klj_file_readv
#define fread klj_file_fread
#define fgetc klj_file_fgetc
#undef getc
#define getc klj_file_fgetc
#define fgets klj_file_fgets
