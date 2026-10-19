#pragma once
#include <binder/IBinder.h>
#include <binder/RpcSession.h>
namespace android {
status_t klj_rpc_transact(const sp<RpcSession>& session,const sp<IBinder>& target,uint32_t code,const Parcel& data,Parcel* reply,uint32_t flags);
bool klj_rpc_authorize_connection(const sp<RpcSession>& session);
void klj_rpc_forget(const RpcSession* session);
}
