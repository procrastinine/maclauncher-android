// Darwin physical HAL boundary. The original AIDL software owner is supplied by the service host.
#pragma once
#include <aidl/android/hardware/security/keymint/IKeyMintDevice.h>
#include <aidl/android/hardware/security/secureclock/ISecureClock.h>
#include <aidl/android/hardware/security/sharedsecret/ISharedSecret.h>
#include <android/binder_ibinder.h>
namespace klepton::keystore {
using Device = aidl::android::hardware::security::keymint::IKeyMintDevice;
using SecurityLevel = aidl::android::hardware::security::keymint::SecurityLevel;
binder_status_t bindSoftwareDevice(const std::shared_ptr<Device>& device);
std::shared_ptr<Device> keyMintDevice(SecurityLevel level);
std::shared_ptr<aidl::android::hardware::security::sharedsecret::ISharedSecret> legacySharedSecret(SecurityLevel);
std::shared_ptr<aidl::android::hardware::security::secureclock::ISecureClock> legacySecureClock(SecurityLevel);
}
// Borrows binder; retains the resulting original interface for process lifetime.
extern "C" binder_status_t kl_keystore_bind_software_keymint(AIBinder* binder);
