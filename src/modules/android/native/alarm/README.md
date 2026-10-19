# Alarm native module

The complete AOSP `libalarm_jni` module is compiled from the Java framework's
pinned source revision. Its original `Android.bp` supplies both implementation
units and the complete JNI table remains intact.

The module retains Android's clock selection, timer ownership, delivery masks,
remaining-time queries and realtime-change handling. Its descriptor operations
use the same epoll/timerfd owner as native applications. The portability header
converts Darwin clock and flag representations, and JNI errors use Android errno
values. There is no separate timer thread or descriptor registry in this module.

`EPOLLWAKEUP` follows Linux's unprivileged behavior: the shared descriptor bridge
accepts the hint without creating a suspend-blocking capability. Delivered work
uses the application's logical Android wake locks. Alarm delivery does not
prevent Mac sleep or schedule the computer to power on or resume.

The library resolves through the existing framework-library loader. Service
startup and Android alarm policy remain separate from this native module.
