#include "PlatformHal.h"
#include <aidl/android/hardware/security/keymint/KeyMintHardwareInfo.h>
#include <mutex>
namespace klepton::keystore {
namespace {
std::mutex softwareMutex;
std::shared_ptr<Device> software;
}
binder_status_t bindSoftwareDevice(const std::shared_ptr<Device>& device) {
    if (!device) return STATUS_BAD_VALUE;
    aidl::android::hardware::security::keymint::KeyMintHardwareInfo info;
    auto status = device->getHardwareInfo(&info);
    if (!status.isOk()) return status.getStatus() == STATUS_OK ? STATUS_BAD_VALUE : status.getStatus();
    if (info.securityLevel != SecurityLevel::SOFTWARE) return STATUS_BAD_VALUE;
    std::lock_guard lock(softwareMutex);
    if (software && software->asBinder().get() != device->asBinder().get()) return STATUS_ALREADY_EXISTS;
    software = device;
    return STATUS_OK;
}
std::shared_ptr<Device> keyMintDevice(SecurityLevel level) {
    if (level != SecurityLevel::SOFTWARE) return {};
    std::lock_guard lock(softwareMutex);
    return software;
}
std::shared_ptr<aidl::android::hardware::security::sharedsecret::ISharedSecret>
legacySharedSecret(SecurityLevel) { return {}; }
std::shared_ptr<aidl::android::hardware::security::secureclock::ISecureClock>
legacySecureClock(SecurityLevel) { return {}; }
}
extern "C" binder_status_t kl_keystore_bind_software_keymint(AIBinder* binder) {
    if (!binder) return STATUS_BAD_VALUE;
    AIBinder_incStrong(binder);
    return klepton::keystore::bindSoftwareDevice(
        klepton::keystore::Device::fromBinder(ndk::SpAIBinder(binder)));
}
