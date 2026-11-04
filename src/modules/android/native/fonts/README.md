# Android font resources

`stageAndroidFonts({sources, output})` installs the original Android prebuilt font modules and font configuration into `output/android-root/system/fonts` and `output/android-root/system/etc`. `sources` maps source keys to read-only Git checkouts; omitted paths use the submodules listed in `sources.json`. Revisions come from the native runtime's `source-pins.json`.

The build reads original `Android.bp` declarations, includes every prebuilt font module in these source packages, follows required module references, and preserves compatibility symlinks. Versioned Noto modules use the original SDK default versions when no Android release build flags are available. Font binaries and XML configuration are copied without modification. The original Android `SystemFonts` and `Typeface` implementations own family parsing and fallback selection.

Licence texts are collected from the source checkouts. The result contains `root` and an inventory of source revisions, module definitions, copied resources and checksums. Licence texts use the shared flat `licenses/` directory and component index. The build fails for an unresolved font configuration reference, required module or source revision; missing licence texts emit warnings.
