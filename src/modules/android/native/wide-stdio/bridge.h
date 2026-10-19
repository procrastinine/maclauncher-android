#pragma once
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

// A formatting operation borrows a stream or a caller's output buffer. It never
// owns or copies FILE internals; those remain with the shared stdio adapter.
typedef struct {
    void *stream;
    wchar_t *buffer;
    size_t capacity, written;
    int error;
} KLJWideOutput;
#ifdef __cplusplus
extern "C" {
#endif
wint_t klj_wide_put(wchar_t, KLJWideOutput *);
int klj_wide_can_write(KLJWideOutput *);
wchar_t *klj_wide_mbsconv(const char *, int);
int *klj_wide_errno(void);
char *klj_wide_radix(void);
char *klj_wide_strerror(int, char *, size_t);
const char *klj_wide_error_name(int);
void *klj_wide_mmap(void *,size_t,int,int,int,int64_t);
__attribute__((noreturn)) void klj_wide_fatal(const char *,...);
#ifdef __cplusplus
}
#endif
