#pragma once
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <stdbool.h>
#include "media-data-source.h"
#include "media-format.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct KLJExtractor KLJExtractor;
KLJExtractor *klj_extractor_create(void);
void klj_extractor_retain(KLJExtractor *);
void klj_extractor_release(KLJExtractor *);
void klj_extractor_close(KLJExtractor *);
int klj_extractor_set_path(KLJExtractor *,const char *,const char *const *,const char *const *,size_t);
int klj_extractor_set_fd(KLJExtractor *,int,int64_t,int64_t);
int klj_extractor_set_source(KLJExtractor *,klj_media_source *);
size_t klj_extractor_count(KLJExtractor *);
AMediaFormat *klj_extractor_format(KLJExtractor *,int); // -1 = file metadata; caller deletes
int klj_extractor_select(KLJExtractor *,size_t,bool);
int klj_extractor_seek(KLJExtractor *,int64_t,int);
int klj_extractor_advance(KLJExtractor *);
ssize_t klj_extractor_read(KLJExtractor *,void *,size_t);
int64_t klj_extractor_sample_time(KLJExtractor *);
int klj_extractor_sample_time_status(KLJExtractor *,int64_t *); // 0=value, 1=EOS, negative error
int klj_extractor_sample_format(KLJExtractor *,AMediaFormat *); // 0=filled, 1=EOS, negative error
int64_t klj_extractor_sample_size(KLJExtractor *);
int klj_extractor_sample_track(KLJExtractor *);
int klj_extractor_sample_flags(KLJExtractor *);
int64_t klj_extractor_cached_duration(KLJExtractor *,bool *);
void klj_extractor_log_session(KLJExtractor *,const char *);
#ifdef __cplusplus
}
#endif
