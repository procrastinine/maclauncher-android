#pragma once
#include <binder/IBinder.h>
#include <binder/Parcel.h>
#include <binder/RpcSession.h>

namespace android {
// Private to the two owned RPC endpoints; removed before original Binder dispatch.
inline constexpr uint32_t kHostParcelEnvelope = 0x40000000;
class HostParcelTransport {
public:
    static status_t encode(const Parcel&, const sp<RpcSession>&, Parcel*);
    static status_t decode(const Parcel&, Parcel*);
    static status_t encodeReply(const Parcel&, const Parcel&, Parcel*);
    static status_t transact(IBinder*, uint32_t, const Parcel&, Parcel*, uint32_t);
};
}
