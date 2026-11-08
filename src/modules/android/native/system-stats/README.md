# Android statistics services

Android's complete original `service-statsd` Java module runs in the shared system
process, and its original Soong compiler products supply every class. The
supplied framework keeps the AIDL interfaces, `framework-statsd` clients,
`StatsHelper` and module utilities, and the build does not create second copies
of them.

`StatsCompanion.Lifecycle` publishes the original `statsmanager` and
`statscompanion` Binder services. Upstream code handles permissions,
pull-callback registrations, configuration subscriptions, the connection to the
native daemon and that daemon's death. `HostStats` connects the lifecycle to
process startup and stops its original worker when the process shuts down.

The native statistics collector is separate from callback registration. When
that daemon is absent, the original service keeps registrations and returns its
normal unavailable result for collector operations. It does not fabricate
measurements or invoke callbacks as though a collection had occurred. Android
media metrics likewise has its own `media.metrics` Binder service.

The source revision and module ownership are recorded in the runtime's common
component notices. The original module uses the Apache-2.0 license.
