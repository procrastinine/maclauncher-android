#include "asset-files.h"
#include <android-base/unique_fd.h>
#include <androidfw/AssetsProvider.h>
#include <errno.h>

extern "C" int klj_asset_open_fd(const char *,int64_t *,int64_t *);

// Loose prepared assets and indexed expansion-file windows become ordinary
// upstream FileAssets. Android owns reads, seeking, mapping and descriptor
// duplication; the platform boundary supplies only an owned physical FD/range.
std::unique_ptr<android::Asset> klj_asset_file(const char *name,android::Asset::AccessMode mode){
    (void)mode;
    int64_t offset=0,length=0;
    android::base::unique_fd fd(klj_asset_open_fd(name,&offset,&length));
    if(fd.get()<0)return {};
    // Retain the exact descriptor instead of asking Android to reopen the
    // guest-relative name. This also preserves unlinked OBB-backed windows.
    return android::AssetsProvider::CreateAssetFromFd(std::move(fd),nullptr,offset,length);
}
