#pragma once

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <unistd.h>

// Both VM hosts publish this existing physical Android log owner. Standalone
// CPU consumers may omit it; their ordinary stderr remains the fallback.
extern "C" int klj_android_log_write(int, const char*, const char*) __attribute__((weak_import));

namespace Dynarmic::Backend {
inline void ReportException(const char* format, ...) noexcept {
    const int saved = errno;
    char message[2048];
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (length >= 0) {
        if (klj_android_log_write) {
            klj_android_log_write(6, "Dynarmic", message);
        } else {
            // A guest may close stderr. A failed diagnostic must not throw
            // through the Mach exception server or change its reply.
            const auto bytes = static_cast<std::size_t>(length) < sizeof(message)
                ? static_cast<std::size_t>(length) : sizeof(message) - 1;
            (void)::write(STDERR_FILENO, message, bytes);
        }
    }
    errno = saved;
}
}
