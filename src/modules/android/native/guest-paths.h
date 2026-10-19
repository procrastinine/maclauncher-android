#ifndef MACLAUNCHER_ANDROID_GUEST_PATHS_H
#define MACLAUNCHER_ANDROID_GUEST_PATHS_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
// NULL means this path belongs to the upstream resolver. A mapped path lives
// in the caller's buffer, never in a shared or transient configuration object.
const char *klj_mapped_guest_path(const char *path,char *buffer,size_t capacity);
/* Reverse only configured namespace aliases. A host file outside that view
 * has no guest pathname; it remains anonymous in process memory metadata. */
const char *klj_guest_path_of_host(const char *path,char *buffer,size_t capacity);
typedef struct KljGuestPathView KljGuestPathView;
/* One bounded namespace snapshot, including the kernel's canonical spelling
 * of existing physical targets. No mapping guard spans path resolution. */
KljGuestPathView *klj_guest_path_view(void);
const char *klj_guest_path_view_name(const KljGuestPathView *,const char *,char *,size_t);
void klj_guest_path_view_release(KljGuestPathView *);
#ifdef __cplusplus
}
#endif
#endif
