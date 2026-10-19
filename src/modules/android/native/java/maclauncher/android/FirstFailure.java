package maclauncher.android;

import java.lang.ref.WeakReference;
import java.util.IdentityHashMap;

/** Bounded diagnostics; exception and guest cleanup behavior remain unchanged. */
public final class FirstFailure {
    private enum Role { OWNER, WORKER }
    private static final int LIMIT = 8192;
    private static WeakReference<Throwable> first, owner, worker;

    static void report(Throwable failure) { reportOwner(failure); }
    public static void reportOwner(Throwable failure) { report(Thread.currentThread(), failure, Role.OWNER); }
    static void reportWorker(Thread thread, Throwable failure) { report(thread, failure, Role.WORKER); }

    private static boolean same(WeakReference<Throwable> ref, Throwable value) {
        return ref != null && ref.get() == value;
    }

    private static synchronized int reserve(Throwable failure, Role role) {
        if (failure == null || same(first, failure) || same(owner, failure)) return 0;
        // A background SDK error is diagnostic, not the host's fatal error.
        // If that same throwable later reaches the owner, it must still report.
        if (role == Role.WORKER) {
            if (worker != null) return 0;
            worker = new WeakReference<>(failure);
            return 3;
        }
        boolean primary = first == null;
        if (primary) first = new WeakReference<>(failure);
        else {
            if (owner != null) return 0;
            owner = new WeakReference<>(failure);
        }
        return primary ? 1 : 2;
    }

    private static void report(Thread thread, Throwable failure, Role role) {
        int slot = reserve(failure, role);
        if (slot == 0) return;
        // Guest Throwable accessors may block or reenter. Reserve the bounded
        // report slot first; never hold the shared lock while invoking them.
        try {
            Text out = new Text();
            out.add(slot == 3 ? "[android-host] Background worker error:\n" : slot == 1 ? "[android-host] Failure:\n" : "[android-host] Secondary failure:\n");
            describe(out, failure);
            out.add("\n[thread id="); out.add(Long.toString(thread.threadId()));
            out.add(" name="); out.lineValue(thread.getName(), 128);
            out.add(" role="); out.add(role == Role.OWNER ? "owner" : "worker"); out.add("]\n");
            trace(out, failure, "", new IdentityHashMap<>(), 0);
            String text = out.finish();
            try {
                // The original Android logger owns physical output separately
                // from guest stdio. Its original line writer also preserves
                // the bounded report across the native logger payload limit.
                android.util.Log.printlns(android.util.Log.LOG_ID_SYSTEM,
                        android.util.Log.ERROR, "AndroidHost", text, null);
            } catch (Throwable unavailable) {
                // Early bootstrap can precede native logging registration.
                try { System.err.print(text); System.err.flush(); } catch (Throwable ignored) { }
            }
        } catch (Throwable ignored) {
            // A diagnostic must never replace the original throwable or prevent cleanup.
        }
    }

    private static void describe(Text out, Throwable failure) {
        out.add(failure.getClass().getName());
        try { String message = failure.getMessage(); if (message != null) { out.add(": "); out.value(message, 1024); } }
        catch (Throwable error) { out.add(" [message unavailable: "); out.add(error.getClass().getName()); out.add("]"); }
    }

    private static void trace(Text out, Throwable failure, String indent, IdentityHashMap<Throwable,Boolean> seen, int depth) {
        if (out.full()) return;
        if (depth >= 8 || seen.size() >= 16) { out.add(indent + "[cause/exception limit]\n"); return; }
        if (seen.put(failure, Boolean.TRUE) != null) { out.add(indent + "[circular exception]\n"); return; }
        try {
            StackTraceElement[] frames = failure.getStackTrace();
            if (frames != null) {
                int count = Math.min(32, frames.length);
                for (int i = 0; i < count && !out.full(); i++) { out.add(indent + "\tat "); out.add(String.valueOf(frames[i])); out.add("\n"); }
                if (count < frames.length) out.add(indent + "\t[stack limit]\n");
            }
        } catch (Throwable error) { out.add(indent + "[stack unavailable: "); out.add(error.getClass().getName()); out.add("]\n"); }
        try {
            Throwable[] suppressed = failure.getSuppressed();
            for (int i = 0; i < Math.min(4, suppressed.length) && !out.full(); i++) {
                out.add(indent + "Suppressed: "); describe(out, suppressed[i]); out.add("\n");
                trace(out, suppressed[i], indent + "\t", seen, depth + 1);
            }
            if (suppressed.length > 4) out.add(indent + "[suppressed limit]\n");
        } catch (Throwable error) { out.add(indent + "[suppressed unavailable]\n"); }
        try {
            Throwable cause = failure.getCause();
            if (cause != null && !out.full()) {
                out.add(indent + "Caused by: "); describe(out, cause); out.add("\n");
                trace(out, cause, indent, seen, depth + 1);
            }
        } catch (Throwable error) { out.add(indent + "[cause unavailable: "); out.add(error.getClass().getName()); out.add("]\n"); }
    }

    private static final class Text {
        private static final String TRUNCATED = "\n[Android error report truncated]\n";
        private final StringBuilder text = new StringBuilder(1024);
        private int bytes;
        private boolean truncated;
        boolean full() { return truncated; }
        void add(String value) {
            if (truncated || value == null) return;
            for (int i = 0; i < value.length();) {
                int cp = value.codePointAt(i), units = Character.charCount(cp);
                if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd;
                int size = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
                if (bytes + size > LIMIT - TRUNCATED.length()) { truncated = true; return; }
                text.appendCodePoint(cp); bytes += size; i += units;
            }
        }
        void lineValue(String value, int limit) {
            int count = Math.min(value.length(), limit);
            for (int i = 0; i < count && !full(); i++) { char c = value.charAt(i); add(c < 0x20 || c == 0x7f ? "?" : String.valueOf(c)); }
            if (value.length() > limit) add("...");
        }
        void value(String value, int limit) {
            int end = Math.min(value.length(), limit);
            if (end > 0 && end < value.length() && Character.isHighSurrogate(value.charAt(end - 1))) end--;
            add(value.substring(0, end));
            if (end < value.length()) add(" [message truncated]");
        }
        String finish() { if (truncated) text.append(TRUNCATED); return text.toString(); }
    }

    /** Scope only our default handler; a handler installed by guest code is preserved. */
    static final class Workers implements AutoCloseable {
        private final Thread ownerThread = Thread.currentThread();
        private final Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        private final Thread.UncaughtExceptionHandler handler = (thread, error) -> {
            // Native render failures already propagate to Host on the owner.
            // Preserve the ordinary Java end-of-thread behavior for SDK workers.
            report(thread, error, thread == ownerThread ? Role.OWNER : Role.WORKER);
            if (previous != null) previous.uncaughtException(thread, error);
        };
        private boolean closed;
        private Workers() { Thread.setDefaultUncaughtExceptionHandler(handler); }
        @Override public void close() {
            if (closed) return; closed = true;
            if (Thread.getDefaultUncaughtExceptionHandler() == handler) Thread.setDefaultUncaughtExceptionHandler(previous);
        }
    }
    static Workers captureWorkers() { return new Workers(); }
}
