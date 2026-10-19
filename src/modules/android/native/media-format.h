#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct AMediaFormat;
typedef struct AMediaFormat AMediaFormat;
/* ABI used by the macOS demuxer/decoder. Public NDK symbols are resolved from
 * the complete upstream media library, independently of these internal users. */
typedef struct {
    AMediaFormat *(*create)(void);
    void (*setInt32)(AMediaFormat *,const char *,int32_t);
    void (*setInt64)(AMediaFormat *,const char *,int64_t);
    void (*setFloat)(AMediaFormat *,const char *,float);
    void (*setString)(AMediaFormat *,const char *,const char *);
    bool (*getString)(AMediaFormat *,const char *,const char **);
} KLJMediaFormatApi;
bool klj_media_format_install(const KLJMediaFormatApi *,void *library);
const KLJMediaFormatApi *klj_media_formats(void);
void *klj_media_format_lookup(const char *name);
const char *klj_media_format_string(AMediaFormat *,const char *);
#ifdef __cplusplus
}
#endif
