# MacLauncher Android

Android and VR/XR runtime components used by [MacLauncher](https://github.com/procrastinine/maclauncher) on Apple Silicon macOS. Application code runs through a Java 21 JVM or ART, with shared Android framework services and native macOS graphics, audio and input.

This repository contains the runtime source, exact upstream dependency pins and MacLauncher adaptations. [MacLauncher Builds](https://github.com/procrastinine/maclauncher-builds) publishes the custom common and ART runtime packages. Installation, version selection, accounts and game settings belong to MacLauncher.

The [main MacLauncher repository](https://github.com/procrastinine/maclauncher) describes features and usage. [Building](docs/BUILDING.md), the [source and runtime reference](docs/REFERENCE.md) and the [Android architecture](docs/runtimes/ANDROID_ARCHITECTURE.md) describe the producer inputs and runtime boundaries.

## Why this exists

The runtime combines established Android execution and native integration projects through shared application services:

- [dex2jvm](https://github.com/kksimp/dex2jvm), also used by [Mac.apk](https://github.com/kksimp/Mac.apk), converts application DEX for JVM execution. The ART binding executes original DEX through [AIM](https://github.com/hahnlee/aim)'s Android Runtime port and its matching Android framework and native libraries.
- [Robolectric](https://github.com/robolectric/robolectric) supplies the executable `android-all` framework and compatible native components. Its test runner and shadow services are separate from the application runtime.
- [Klepton](https://github.com/shinyquagsire23/Klepton) supplies native Android loading, ABI adaptation and VR/XR integration. Native libraries use the actual JVM or ART for Java objects, callbacks and JNI registration.

Android framework classes, members and resources remain present for the selected framework and ABI profile. Platform adaptations connect their operating-system dependencies to the shared native owners. Surface, image, window, audio and application-operation lifetimes use those same owners across the two execution bindings.

The application boundary is game-focused. Each game has private writable storage and explicit imported files; network, clipboard, device and account capabilities have separate policy. OpenXR and OVRPlugin entry points share the native graphics and input implementation with ordinary Android applications.

[microG](https://github.com/microg/GmsCore) supplies optional Google Play services integration. MacLauncher owns account selection and authorization; game processes receive scoped replies rather than access to the launcher account store.

MacLauncher's optional Mac.apk and standalone AIM providers have their own acquisition and process boundaries. Mac.apk uses its publisher releases. Adapted AIM packages use MacLauncher Builds and acquire the original compatible Android system image separately. The shared ART binding loads its matching framework, libcore and boot image into an application host; the standalone AIM provider boots an Android services guest.
