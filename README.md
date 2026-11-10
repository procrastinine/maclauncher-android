# MacLauncher Android

> **[MacLauncher](https://github.com/procrastinine/maclauncher)** plays Windows, PS5, Switch, Android and iOS games natively on macOS, with ReShade, DLSS, MetalFX and Lossless Scaling effects, Steam downloads and version management for every runtime. This repository is one of its components. The [MacLauncher website](https://procrastinine.github.io/maclauncher/) describes its features, and the app is on the [MacLauncher releases page](https://github.com/procrastinine/maclauncher/releases).

MacLauncher Android contains the Android and VR/XR runtime components that [MacLauncher](https://github.com/procrastinine/maclauncher) uses on Apple Silicon Macs. Android application code runs on a Java 21 JVM or on ART, with shared Android framework services and native macOS graphics, audio and input.

This repository holds the runtime source, the exact upstream commit of each dependency and MacLauncher's adaptations. [MacLauncher Builds](https://github.com/procrastinine/maclauncher-builds) builds the common (JVM) and ART runtime packages from it and publishes them. MacLauncher itself handles installation, version selection, accounts and game settings.

The [main MacLauncher repository](https://github.com/procrastinine/maclauncher) describes features and usage. [Building](docs/BUILDING.md), the [source and runtime reference](docs/REFERENCE.md), the [Android runtime architecture](docs/runtimes/ANDROID_ARCHITECTURE.md) and [Android native code](docs/runtimes/ANDROID_NATIVE_CODE.md) describe the build inputs and how the runtime fits together.

## How it works

The runtime combines established Android execution and native integration projects, which share one set of application services:

- [dex2jvm](https://github.com/kksimp/dex2jvm) converts application DEX bytecode for JVM execution; [Mac.apk](https://github.com/kksimp/Mac.apk) also uses it. The ART binding runs the original DEX on [AIM](https://github.com/hahnlee/aim)'s port of the Android Runtime, with its matching Android framework and native libraries.
- [Robolectric](https://github.com/robolectric/robolectric) supplies the executable `android-all` framework and compatible native components. Its test runner and shadow services are not part of the application runtime.
- [Klepton](https://github.com/shinyquagsire23/Klepton) supplies native Android library loading, ABI adaptation and VR/XR integration. Native libraries use the real JVM or ART for Java objects, callbacks and JNI registration.

Every Android framework class, member and resource of the selected framework version and ABI stays present. Platform adaptations connect the framework's operating-system dependencies to shared native implementations, and both execution bindings manage surfaces, images, windows, audio and application operations through those same implementations.

The runtime is built for games: each game has private writable storage and sees only the files explicitly imported into it, and network, clipboard, device and account access each follow their own policy. OpenXR and OVRPlugin entry points share the native graphics and input implementation with ordinary Android applications.

[microG](https://github.com/microg/GmsCore) supplies optional Google Play services integration. MacLauncher handles account selection and authorization, and game processes receive scoped replies without access to the launcher's account store.

The ART binding loads its matching framework, libcore and boot image into an application host process. This repository also holds the integration for MacLauncher's optional standalone AIM runtime, which boots an Android services guest and uses a separately downloaded Android system image. Mac.apk, MacLauncher's other optional Android runtime, comes from its publisher and is not built from this repository.
