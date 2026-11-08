# Android video device

Android's original media libraries handle media formats, extraction, codec sessions, buffer queues and their Java and NDK interfaces. Original Codec2 components use the VideoToolbox device for hardware decoding alongside the original software codecs.

A Mac device library publishes hardware codec capabilities and typed decoder operations through the shared native import registry. It holds the VideoToolbox sessions and pixel buffers, while Android media objects stay in the original libraries. The original media codec constants header supplies the profile and level values in the capability catalog.

Android media-metrics producers submit their original serialized buffers to the `media.metrics` Binder service, and the JVM native boundary carries the same one-way `IMediaMetricsService.submitBuffer` transaction. When no collector is registered, the transaction returns Android's normal `NO_INIT` status. Playback and audio focus do not depend on telemetry collection.
