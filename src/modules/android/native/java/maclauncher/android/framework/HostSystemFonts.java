package maclauncher.android.framework;

import android.graphics.Typeface;
import android.os.SharedMemory;
import android.system.ErrnoException;
import java.io.IOException;

/** Publishes the original process font map before application UI is created. */
public final class HostSystemFonts {
    private HostSystemFonts() {}

    public static synchronized void initialize(SharedMemory serialized) {
        // A VM may have already published the original map during native
        // registration. Keep its Typeface objects and backing memory intact.
        if (!Typeface.getSystemFontMap().isEmpty()) return;
        try {
            Typeface.setSystemFontMap(serialized);
        } catch (IOException | ErrnoException failure) {
            // ActivityThread uses the original preinstalled configuration when
            // a serialized system-server map cannot be read.
            Typeface.loadPreinstalledSystemFontMap();
        }
    }
}
