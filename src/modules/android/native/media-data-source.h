#ifndef MACLAUNCHER_MEDIA_DATA_SOURCE_H
#define MACLAUNCHER_MEDIA_DATA_SOURCE_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct klj_media_source klj_media_source;
typedef ssize_t (*klj_media_read)(void *,int64_t,void *,size_t);
typedef ssize_t (*klj_media_size)(void *);
typedef ssize_t (*klj_media_available)(void *,int64_t);
typedef void (*klj_media_close)(void *);

klj_media_source *klj_media_source_new(void);
void klj_media_source_retain(klj_media_source *);
void klj_media_source_delete(klj_media_source *);
void klj_media_source_close(klj_media_source *);
void klj_media_source_userdata(klj_media_source *,void *);
void klj_media_source_set_read(klj_media_source *,klj_media_read);
void klj_media_source_set_size(klj_media_source *,klj_media_size);
void klj_media_source_set_close(klj_media_source *,klj_media_close);
/* Disposal runs once after the last request/owner reference retires. */
void klj_media_source_set_dispose(klj_media_source *,klj_media_close);
klj_media_source *klj_media_source_from_fd(int,int64_t,int64_t);
void klj_media_source_set_available(klj_media_source *,klj_media_available);
ssize_t klj_media_source_read(klj_media_source *,int64_t,void *,size_t);
ssize_t klj_media_source_size(klj_media_source *);
int klj_media_source_closed(klj_media_source *);
void *klj_media_source_lookup(const char *);
#ifdef __cplusplus
}
#endif
#endif
