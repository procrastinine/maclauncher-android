# MacLauncher Android

The Android runtime used by [MacLauncher](https://github.com/procrastinine/maclauncher). In short, it runs Android and VR/XR games natively on macOS, with JVM and ART execution and native macOS graphics, audio and input on Apple Silicon.

This repository contains the runtime source, pinned upstream dependencies and MacLauncher adaptations. [MacLauncher Builds](https://github.com/procrastinine/maclauncher-builds) publishes its runtime packages; installation, version selection and game settings are handled by MacLauncher.

The [main MacLauncher repository](https://github.com/procrastinine/maclauncher) describes features and usage. [Building](docs/BUILDING.md) and the [technical reference](docs/REFERENCE.md) describe the source and runtime interfaces.

## Why this exists

There are already two projects that I know of that can run Android on macOS: namely, [Mac.apk](https://github.com/kksimp/Mac.apk) and [AIM](https://github.com/hahnlee/aim). Here is a brief description of how this project compares to those.
- First, it uses the "backends" from those projects. Specifically, it runs Android binaries through a choice of [dex2jvm](https://github.com/kksimp/dex2jvm) (used in Mac.apk) and AIM's port of ART (Android Runtime) to macOS. (So you can choose between either one)
- It also uses a lot of existing tools. For example, [Robolectric's](https://github.com/robolectric/robolectric) android-all, and [Klepton's](https://github.com/shinyquagsire23/Klepton) macOS integration.
- It can be said that this is conceptually "Klepton with a real JVM instead of a fake one". Klepton can run ARM VR/XR games with minimal Java code, and MacLauncher Android basically extends that to everything _including_ heavy Java code, being a superset of Klepton (minus bugs).
- Therefore, from the foundation of Klepton, MacLauncher Android is not only capable of playing Android games, but also VR/XR games like Beat Saber and Steam Frame native games. This is one differentiating factor from Mac.apk and AIM.
- Another difference is the "development philosophy" behind MacLauncher Android. Basically, MacLauncher Android is exclusively about games. I don't care much if Clock or Google Maps are compatible, the only focus of this runtime is that games run.
  - As such, compared to Mac.apk and AIM, the development philosophy is "minimum integration with mac", not "maximum integration". For example, we only allow games to access a specific game data root directory and not the whole mac, we don't provide location access (by default), etc etc. Basically, we treat a game as a single app to run, not a whole system.
  - Practically, I think the game compatibility (especially for commercial games) is better on MacLauncher Android compared to Mac.apk or AIM (not rigorous but just from popular games I have looked at). I have a testing set of like 90-some Android games and more than half run to some extent, while only 20-some run in the other runtimes.
- Technically, another difference is that MacLauncher Android integrates [microG](https://github.com/microg/GmsCore) for Google Play services.