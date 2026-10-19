# Android statistics services

The complete original `service-statsd` Java module runs in the profile's system
process. Its original Soong compiler products supply every original class. The supplied
framework retains the AIDL interfaces, `framework-statsd` clients, `StatsHelper`
and module utilities; the build does not create second copies of those owners.

`StatsCompanion.Lifecycle` publishes the original `statsmanager` and
`statscompanion` Binder services. Upstream code owns permissions, pull-callback
registrations, configuration subscriptions, native-daemon connection and death
handling. `HostStats` connects that lifecycle to process startup and stops its
original worker during process retirement.

The native statistics collector is separate from callback registration. When
that daemon is absent, the original service retains registrations and reports
its normal unavailable result for collector operations. It does not fabricate
measurements or invoke callbacks as though a collection occurred. Android media
metrics similarly uses its own `media.metrics` Binder producer boundary.

Source revision and module ownership accompany the runtime in its common
component notices. The original module uses the Apache-2.0 license.
