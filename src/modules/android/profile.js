// The staged manifest declares only contracts implemented by its exact host.
function runtimeProfile(pins) {
  const profile = { id: "klepton-jvm-" + pins.version + "-arm64", javaMajor: pins.javaMajor,
    nativeLoadTarget: "maclauncher.android.bridge.GuestNativeLoader" };
  const contracts = [
    ["artExecutionVersion", "ART execution", ["frameworkServicesVersion", "splitApksVersion"]],
    ["arm32Version", "ARM32 native execution", ["frameworkBaseVersion"]],
    ["frameworkBaseVersion", "AOSP framework", []],
    ["conversionMetadataVersion", "conversion metadata", ["frameworkBaseVersion"]],
    ["demandDexVersion", "on-demand DEX conversion", ["frameworkBaseVersion", "conversionMetadataVersion"]],
    ["applicationMetadataVersion", "application metadata", []],
    ["packageMetadataVersion", "package names and icons", ["frameworkBaseVersion"]],
    ["applicationThemeVersion", "application themes", ["applicationMetadataVersion"]],
    ["applicationInfoVersion", "application identity", ["applicationMetadataVersion"]],
    ["applicationMetadataEntriesVersion", "application metadata entries", ["applicationMetadataVersion"]],
    ["activityMetadataVersion", "activity metadata", ["applicationMetadataEntriesVersion"]],
    ["activityInfoVersion", "activity information", ["applicationMetadataEntriesVersion"]],
    ["processExitHistoryVersion", "process history", []],
    ["webLinksVersion", "web links", []],
    ["adPolicyVersion", "advertising policy", []],
    ["webViewVersion", "web views", ["frameworkBaseVersion", "canvasPresentationVersion"]],
    ["servicePackagesVersion", "installed service packages", ["frameworkBaseVersion"]],
    ["providerConnectionsVersion", "Android process connections", ["servicePackagesVersion"]],
    ["processAuthorityVersion", "Android process lifecycle", ["providerConnectionsVersion"]],
    ["processIdentityVersion", "Android process identity", ["processAuthorityVersion"]],
    ["platformServicesVersion", "Android platform services", ["processAuthorityVersion", "frameworkBaseVersion"]],
    ["systemServicesVersion", "Android system services", ["platformServicesVersion"]],
    ["lazyProfileServicesVersion", "on-demand profile services", ["platformServicesVersion", "systemServicesVersion", "sharedAccountsVersion"]],
    ["accountControlVersion", "account service control", ["servicePackagesVersion"]],
    ["accountActivityVersion", "account sign-in activities", ["accountControlVersion", "processAuthorityVersion"]],
    ["sharedAccountsVersion", "shared account selection", ["accountActivityVersion", "platformServicesVersion"]],
    ["frameworkServicesVersion", "shared framework service ownership", ["sharedAccountsVersion", "systemServicesVersion"]],
    ["googleProviderCompatibilityVersion", "Google provider compatibility", ["servicePackagesVersion"]],
    ["playLibraryVersion", "Google Play library actions", ["sharedAccountsVersion", "googleProviderCompatibilityVersion"]],
    ["mediaInputsVersion", "media inputs", []],
    ["manualSensorVersion", "manual motion input", ["frameworkBaseVersion"]],
    ["keyboardTouchVersion", "keyboard touch controls", ["frameworkBaseVersion"]],
    ["producerResolutionVersion", "render resolution", ["canvasPresentationVersion"]],
    ["angleVulkanVersion", "Vulkan EGL rendering", ["frameworkBaseVersion"]],
    ["splitApksVersion", "split APK packages", []],
    ["applicationComponentsVersion", "application components", ["applicationMetadataVersion", "applicationMetadataEntriesVersion"]],
    ["canvasPresentationVersion", "Canvas presentation", []]
  ];
  for (const [key, label, required] of contracts) {
    if (pins[key] == null) continue;
    if (pins[key] !== 1 || required.some(name => pins[name] !== 1))
      throw new Error("Unsupported Android " + label + " profile.");
    profile[key] = pins[key];
  }
  // This owned source generation compiles the JVM application/system loader
  // together with the full framework. Older builds retain their own snapshot
  // of this recipe and therefore their original startup capability.
  if (profile.frameworkBaseVersion === 1) profile.applicationSystemClassLoaderVersion = 1;
  return profile;
}

module.exports = { runtimeProfile };
