# Service Application ownership

Local services use the Application attached to the original LoadedApk of the
process they are bound to. The caller's Context resolves the requested component
but does not choose the service's Application. Before the original
AppComponentFactory creates the service, the component metadata must match the
bound package, UID and process. Service.attach compares the supplied Application
with that exact original LoadedApk instance. If the original process records
adopt another Application, the initial entry Application that the host keeps
does not become a second owner of services.

Service creation runs on the application lifecycle handler after the
Application is attached. The original LoadedApk dispatchers keep the binding and
rebind callbacks. Stopping or shutting down the owning Application keeps normal
Service cleanup and removes queued creation work. Remote service routing keeps
its own process owner.
