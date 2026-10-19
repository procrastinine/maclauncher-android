#pragma once
#include <stddef.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct KljDirectoryCursor KljDirectoryCursor;
KljDirectoryCursor *klj_directory_cursor_create(int);
/* Consumes metadata on every outcome. The caller serializes aliases of the
 * same storage with its existing FileDescription mutex. */
KljDirectoryCursor *klj_directory_cursor_import(int,int);
int klj_directory_cursor_matches(KljDirectoryCursor *,int);
int klj_directory_cursor_metadata(KljDirectoryCursor *);
void klj_directory_cursor_destroy(KljDirectoryCursor *);
ssize_t klj_directory_cursor_read(KljDirectoryCursor *,int,void *,size_t);
off_t klj_directory_cursor_seek(KljDirectoryCursor *,int,off_t,int);
#ifdef __cplusplus
}
#endif
