package maclauncher.android.framework;

import com.android.internal.os.ApplicationSharedMemory;

/** Process-wide shared-memory ownership for the local Android services. */
public final class HostApplicationSharedMemory {
    private HostApplicationSharedMemory() {}

    public static synchronized void initialize() {
        if (ApplicationSharedMemory.sInstance != null) {
            if (!ApplicationSharedMemory.sInstance.isMutable()) {
                throw new IllegalStateException("Local Android services require writable application shared memory");
            }
            return;
        }
        ApplicationSharedMemory memory = ApplicationSharedMemory.create();
        try {
            ApplicationSharedMemory.setInstance(memory);
        } catch (RuntimeException | Error failure) {
            try { memory.close(); } catch (RuntimeException | Error close) { failure.addSuppressed(close); }
            throw failure;
        }
        // PropertyInvalidatedCache keeps this region's native pointers in process-static
        // handlers. Its lifetime therefore follows the process, not an Application/Looper.
        // The kernel retires the mapping and descriptors when the process exits.
    }
}
