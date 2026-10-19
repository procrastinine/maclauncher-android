#pragma once
#include <stddef.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Immutable for one Android process. This does not grant network access. */
bool klj_network_configure_ads(bool allowed);
bool klj_network_ads_allowed(void);
bool klj_network_host_allowed(const char *hostname);
const char *const *klj_network_ad_hosts(size_t *count);
#ifdef __cplusplus
}
#endif
