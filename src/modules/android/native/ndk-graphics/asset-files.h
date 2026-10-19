#pragma once
#include <androidfw/Asset.h>
std::unique_ptr<android::Asset> klj_asset_file(const char *,android::Asset::AccessMode);
extern "C" void klj_asset_opened(const char *,int,int64_t,int);
