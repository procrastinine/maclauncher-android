# Android HWUI

`buildHwui({work, sources, tools, image})` builds the complete Android 16 ARM64 `libhwui.so` target with its original Skia and static dependencies. The library uses the private ABI of the declared API 36 system image. Shared dependencies are linked from that immutable image, which is not copied into the module output.

`sources.json` declares the original source revisions and the Git objects that the build uses, and identifies the shared-image files. `prepareHwuiInputs` accepts explicit clean checkouts or verified original source subtrees. Build tools and their resource directories are explicit inputs. Derived headers, configuration, objects and libraries stay under `work`, and original sources stay read-only.

The target and Perfetto graphs keep the original source lists and features. The original AIDL, HIDL, aconfig, sysprop, stats and protobuf generators produce their headers and sources, and the host-generator recipes accept the original protobuf and foundation libraries. The Android build keeps the original release compiler defaults, per-target exception settings, sanitizer policy and minimal compiler runtime. Platform libc++ uses its original `__1` ABI namespace.

The CommonPool adaptation keeps the actual joinable worker handles outside the class, so the class layout and public API are unchanged. Shutdown publishes the stopping state under the mutex, wakes the workers and joins them before destroying the queue and synchronization members. If construction fails, workers that were already created are joined before the local setup state unwinds. Finalization from a worker thread joins the other workers and releases the calling handle, and that worker's loop exits before returning to pool storage.

The final link requires all original exported symbols and the original set of shared dependencies, with 16 KiB load alignment. `runtime-files.json` describes `guest/libhwui.so`, and licence texts accompany it in the ART supplier, the directory of ART components from which the ART package is assembled. The original system image stays a separate download.

`buildArt({hwui, ...})` selects this build only when an explicit `hwui` configuration is supplied; that configuration adds the complete source, tool and image inputs. Without `hwui`, the image's original library is kept. Modules that are already built can be passed directly to `assembleArtSupplier` through `inputs.hwuiLibrary`.
