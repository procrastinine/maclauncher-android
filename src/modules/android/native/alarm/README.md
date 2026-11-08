# Alarm native module

This module is the complete AOSP `libalarm_jni`, compiled from the same source
revision as the Java framework. Its original `Android.bp` supplies both implementation
units, and the complete JNI table stays intact.

The module keeps Android's clock selection, timer ownership, delivery masks,
remaining-time queries and realtime-change handling. Its descriptor operations
use the same epoll and timerfd implementation as native applications. The
portability header converts Darwin clock and flag representations, and JNI
errors use Android errno values. The module has no timer thread or descriptor
registry of its own.

`EPOLLWAKEUP` follows Linux's behavior for unprivileged processes: the shared
descriptor bridge accepts the hint without creating a capability that blocks
suspend. Delivered work uses the application's logical Android wake locks.
Alarm delivery neither keeps the Mac awake nor schedules it to power on or
resume.

The library loads through the existing framework-library loader. Service
startup and Android alarm policy are separate from this native module.
