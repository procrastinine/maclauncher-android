#include "ParcelTransport.h"
#include <binder/BpBinder.h>
#include <binder/Binder.h>
#include <binder/Functional.h>
#include <binder/RpcSession.h>
#include "binder_module.h"
#include "OS.h"
#include "Utils.h"
#include "descriptor-capability.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace android {
namespace {
constexpr int32_t kEnvelopeVersion = 1;
constexpr int32_t kBinderObject = 1;
constexpr int32_t kDescriptorObject = 2;
constexpr int32_t kDescriptorCapabilityObject = 3;
const char relayIdentity = 0;

// A Binder exported through another authenticated process remains the same
// object. Original IBinder owns the weak relay attachment; each RPC session
// owns only its normal export references. There is no second object registry.
class Relay final : public BBinder {
    const sp<IBinder> target;
public:
    explicit Relay(sp<IBinder> original):target(std::move(original)){}
    bool checkSubclass(const void* identity) const override {return identity==&relayIdentity;}
    const sp<IBinder>& original() const {return target;}
    status_t onTransact(uint32_t code,const Parcel& data,Parcel* reply,uint32_t flags) override {
        return HostParcelTransport::transact(target.get(),code,data,reply,flags);
    }
};
sp<IBinder> exportBinder(sp<IBinder> binder,const sp<RpcSession>& session){
    if(!binder)return binder;
    if(binder->checkSubclass(&relayIdentity))binder=static_cast<Relay*>(binder.get())->original();
    auto* remote=binder->remoteBinder();
    if(!remote||!remote->isRpcBinder()||remote->hostRpcSession()==session)return binder;
    return binder->lookupOrCreateWeak(&relayIdentity,[](const void* original)->sp<IBinder>{
        return sp<Relay>::make(*static_cast<const sp<IBinder>*>(original));
    },&binder);
}
}

status_t HostParcelTransport::encode(const Parcel& source, const sp<RpcSession>& session, Parcel* output) {
    const auto* fields=source.maybeKernelFields();
    if(!fields||!session||!output)return BAD_VALUE;
    if(source.dataSize()>source.mDataCapacity)return BAD_VALUE;
    std::vector<uint8_t> bytes(source.dataSize());
    auto wipe=binder::impl::make_scope_guard([&]{if(!bytes.empty())zeroMemory(bytes.data(),bytes.size());});
    if(!bytes.empty())std::memcpy(bytes.data(),source.data(),bytes.size());
    std::vector<binder_size_t> offsets(fields->mObjectsSize);
    if(!offsets.empty())std::copy_n(fields->mObjects,fields->mObjectsSize,offsets.begin());
    std::sort(offsets.begin(),offsets.end());
    struct Attachment {size_t offset;int32_t kind;sp<IBinder> binder;binder::unique_fd fd;std::vector<uint8_t> capability;};
    std::vector<Attachment> attachments;attachments.reserve(offsets.size());
    size_t end=0,descriptorCount=0;
    // Native pointers and process-local descriptor numbers never cross the
    // socket. Original Binder RPC carries the actual referenced objects.
    for(size_t offset:offsets){
        if(offset<end||(offset&3)||offset>bytes.size()||bytes.size()-offset<sizeof(flat_binder_object))return BAD_VALUE;
        end=offset+sizeof(flat_binder_object);
        flat_binder_object object;std::memcpy(&object,source.data()+offset,sizeof(object));
        Attachment attachment{offset,0,nullptr,{}};
        if(object.hdr.type==BINDER_TYPE_BINDER){
            attachment.kind=kBinderObject;
            if(object.cookie)attachment.binder=sp<IBinder>::fromExisting(reinterpret_cast<IBinder*>(object.cookie));
            auto* remote=attachment.binder?attachment.binder->remoteBinder():nullptr;
            if(remote&&!remote->isRpcBinder())return INVALID_OPERATION;
            attachment.binder=exportBinder(std::move(attachment.binder),session);
        }else if(object.hdr.type==BINDER_TYPE_FD){
            if(!output->mAllowFds||session->getFileDescriptorTransportMode()==RpcSession::FileDescriptorTransportMode::NONE)return FDS_NOT_ALLOWED;
            int descriptor;status_t status=binder::os::dupFileDescriptor(object.handle,&descriptor);
            if(status!=OK)return status;
            attachment.kind=kDescriptorObject;attachment.fd.reset(descriptor);descriptorCount++;
            int64_t length=klj_ashmem_capability_export(descriptor,nullptr,0);if(length<0)return (status_t)length;
            if(length){attachment.capability.resize(length);if(klj_ashmem_capability_export(descriptor,attachment.capability.data(),attachment.capability.size())!=length)return BAD_VALUE;attachment.kind=kDescriptorCapabilityObject;}
        }else return BAD_TYPE; // No kernel Binder handles exist on Darwin.
        attachments.push_back(std::move(attachment));
        std::memset(bytes.data()+offset,0,sizeof(flat_binder_object));
    }
    bool sensitive=source.mDeallocZero||output->mDeallocZero;
    output->freeData();output->markForRpc(session);if(sensitive)output->markSensitive();
    // Validate and acquire attachments before exporting any Binder references.
    // Original RPC owns exports once written, even if a later operation fails.
    if(bytes.size()>INT32_MAX-20||attachments.size()>(INT32_MAX-20-bytes.size())/28)return NO_MEMORY;
    status_t capacity=output->setDataCapacity(bytes.size()+20+attachments.size()*28);
    if(capacity!=OK)return capacity;
    auto* rpc=output->maybeRpcFields();rpc->mObjectPositions.reserve(attachments.size());
    if(descriptorCount){rpc->mFds=std::make_unique<decltype(rpc->mFds)::element_type>();rpc->mFds->reserve(descriptorCount);}
    status_t status=output->writeInt32(kEnvelopeVersion);
    if(status!=OK)return status;
    if((status=output->writeByteVector(bytes))!=OK)return status;
    if((status=output->writeUint64(fields->mObjectsSize))!=OK)return status;
    for(auto& attachment:attachments){
        if((status=output->writeUint64(attachment.offset))!=OK)return status;
        if((status=output->writeInt32(attachment.kind))!=OK)return status;
        if(attachment.kind==kBinderObject){
            if((status=output->writeStrongBinder(attachment.binder))!=OK)return status;
        }else{
            if((status=output->writeFileDescriptor(attachment.fd.release(),true))!=OK)return status;
            if(attachment.kind==kDescriptorCapabilityObject&&(status=output->writeByteVector(attachment.capability))!=OK)return status;
        }
    }
    output->setDataPosition(0);return OK;
}

status_t HostParcelTransport::decode(const Parcel& source, Parcel* output) {
    if(!source.isForRpc()||!output)return BAD_VALUE;
    const size_t position=source.dataPosition();
    struct Restore {const Parcel& parcel;size_t position;~Restore(){parcel.setDataPosition(position);}} restore{source,position};
    source.setDataPosition(0);
    int32_t version;status_t status=source.readInt32(&version);
    if(status!=OK||version!=kEnvelopeVersion)return BAD_VALUE;
    std::vector<uint8_t> bytes;
    auto wipe=binder::impl::make_scope_guard([&]{if(!bytes.empty())zeroMemory(bytes.data(),bytes.size());});
    if((status=source.readByteVector(&bytes))!=OK)return status;
    uint64_t count;
    if((status=source.readUint64(&count))!=OK)return status;
    if(count>bytes.size()/sizeof(flat_binder_object))return BAD_VALUE;
    bool sensitive=source.mDeallocZero||output->mDeallocZero;
    output->freeData();if(sensitive)output->markSensitive();
    if((status=output->setData(bytes.data(),bytes.size()))!=OK)return status;
    size_t end=0;
    for(uint64_t i=0;i<count;i++){
        uint64_t offset;int32_t kind;
        if((status=source.readUint64(&offset))!=OK||(status=source.readInt32(&kind))!=OK)return status;
        if(offset<end||(offset&3)||offset>bytes.size()||bytes.size()-offset<sizeof(flat_binder_object))return BAD_VALUE;
        output->setDataPosition(offset);end=offset+sizeof(flat_binder_object);
        if(kind==kBinderObject){
            sp<IBinder> binder;if((status=source.readNullableStrongBinder(&binder))!=OK)return status;
            if(binder&&binder->checkSubclass(&relayIdentity))binder=static_cast<Relay*>(binder.get())->original();
            flat_binder_object object{};object.hdr.type=BINDER_TYPE_BINDER;
            object.binder=binder?reinterpret_cast<uintptr_t>(binder->getWeakRefs()):0;
            object.cookie=reinterpret_cast<uintptr_t>(binder.get());
            // This is local native Parcel storage, not kernel transport. Its
            // original acquire/release logic retains this exact IBinder owner.
            if((status=output->writeObject(object,false))!=OK)return status;
        }else if(kind==kDescriptorObject||kind==kDescriptorCapabilityObject){
            int descriptor=source.readFileDescriptor();if(descriptor<0)return BAD_VALUE;
            if(kind==kDescriptorCapabilityObject){std::vector<uint8_t> token;if((status=source.readByteVector(&token))!=OK)return status;
                status=(status_t)klj_ashmem_capability_import(descriptor,token.data(),token.size());if(status!=OK)return status;}
            if((status=output->writeDupFileDescriptor(descriptor))!=OK)return status;
        }else return BAD_TYPE;
    }
    if(source.dataAvail()!=0)return BAD_VALUE;
    output->setDataPosition(0);return OK;
}

status_t HostParcelTransport::transact(IBinder* target,uint32_t code,const Parcel& data,Parcel* reply,uint32_t flags){
    auto* remote=target->remoteBinder();
    if(data.isForRpc()||!remote||!remote->isRpcBinder())return target->transact(code,data,reply,flags);
    Parcel outgoing,incoming;
    outgoing.markForBinder(sp<IBinder>::fromExisting(target));
    const sp<RpcSession> session=outgoing.maybeRpcFields()->mSession;
    status_t status=encode(data,session,&outgoing);
    if(status!=OK)return status;
    status=target->transact(code,outgoing,reply?&incoming:nullptr,flags|kHostParcelEnvelope);
    if(status!=OK||!reply)return status;
    return decode(incoming,reply);
}
status_t HostParcelTransport::encodeReply(const Parcel& request,const Parcel& reply,Parcel* output){
    const auto* fields=request.maybeRpcFields();
    return fields?encode(reply,fields->mSession,output):BAD_VALUE;
}
}
