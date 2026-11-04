# Android HWUI

`buildHwui({work, sources, tools, image})` builds the complete pinned Android 16 ARM64 `libhwui.so` target and its original Skia and static dependencies. The library uses the private ABI of the declared API 36 system image. Shared dependencies are linked from that immutable image; the image is not copied into the module output.

`sources.json` declares the original source revisions, consumed Git objects and shared-image file identities. `prepareHwuiInputs` accepts explicit clean checkouts or verified original source subtrees. Build tools and their resource directories are explicit inputs. Derived headers, configuration, objects and libraries stay under `work`; original sources remain read-only.

The target and Perfetto graphs retain the original source lists and features. Original AIDL, HIDL, aconfig, sysprop, stats and protobuf generators provide their headers and sources. The host-generator recipes accept original protobuf and foundation libraries. The Android build retains the original release compiler defaults, per-target exception settings, sanitizer policy and minimal compiler runtime. Platform libc++ uses its original `__1` ABI namespace.

The CommonPool adaptation retains actual joinable worker handles outside the class, preserving its layout and public API. Shutdown publishes the stopping state under the mutex, wakes workers and joins them before destroying queue and synchronization members. Construction failure joins already-created workers before local setup state unwinds. Finalization from a worker joins the other workers and retires the calling handle; its worker loop exits before returning to pool storage.

The final link requires all original exported symbols and the original shared dependency set, with 16 KiB load alignment. `runtime-files.json` describes `guest/libhwui.so`; licence texts accompany it in the ART supplier. The original system image remains a separate, explicitly acquired dependency.

`buildArt({hwui, ...})` selects this producer only when an explicit `hwui` configuration is supplied. That configuration adds the complete source/tool/image inputs. Omitting `hwui` retains the original image library. Already-built modules can be supplied directly to `assembleArtSupplier` through `inputs.hwuiLibrary`.
