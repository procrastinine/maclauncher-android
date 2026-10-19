# Android video device

Original Android media libraries own media formats, extraction, codec sessions, buffer queues and their Java and NDK interfaces. Original Codec2 components use the VideoToolbox device for hardware decoding, alongside the original software codecs.

The Mac device library publishes hardware codec capabilities and typed decoder operations through the shared native import registry. It retains VideoToolbox sessions and pixel buffers; Android media objects remain in the original libraries. The original media codec constants header supplies the profile and level values used by the capability catalog.

Android media-metrics producers submit their original serialized buffers to the `media.metrics` Binder service. The JVM native boundary carries the same one-way `IMediaMetricsService.submitBuffer` transaction. It returns Android's normal `NO_INIT` status when no collector is registered; playback and audio focus remain independent of telemetry collection.
