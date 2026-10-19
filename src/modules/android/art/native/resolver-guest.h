#pragma once
#include <cstdint>
// The original DnsResolver owns DNS protocol, queries, caches and AIDL state.
// Run before starting Binder/RPC threads, as init prepares control sockets.
bool klj_art_resolver_prepare();
bool klj_art_resolver_start(bool allowed,bool allowAds);
int klj_art_resolver_network(uint32_t netId,int state);
