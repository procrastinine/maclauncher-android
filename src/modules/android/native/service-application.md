# Service Application ownership

Local services use the Application attached to the bound process's original
LoadedApk. The caller Context resolves the requested component; it does not
select the service's Application. Component metadata must match the bound
package, UID and process before the original AppComponentFactory creates the
service. Service.attach compares the supplied Application with that exact original
LoadedApk instance. An initial entry Application retained by the host is not a
second service owner when original process records adopt another Application.

Service creation runs on the application lifecycle handler, after Application
attachment. Original LoadedApk dispatchers retain binding and rebind callbacks;
stopping or retiring the owner preserves normal Service cleanup and removes
queued creation work. Remote service routing keeps its separate process owner.
