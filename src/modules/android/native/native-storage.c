#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

/* Native frameworks ask dirhelper for their scratch directory before consulting
 * TMPDIR. Keep that one process-local query on the same private directory as the
 * JVM and guest. In particular, the Darwin cache query remains unchanged: Metal
 * owns its original per-bundle cache and compiler-service delegation. */
static pthread_once_t configured = PTHREAD_ONCE_INIT;
static char *temporary;
static size_t temporary_size;
static void configure(void) {
    const char *value = getenv("TMPDIR");
    if (!value || value[0] != '/') return;
    size_t length = strlen(value);
    int slash = value[length - 1] != '/';
    temporary = malloc(length + slash + 1);
    if (!temporary) return;
    memcpy(temporary, value, length);
    if (slash) temporary[length++] = '/';
    temporary[length] = 0;
    temporary_size = length + 1;
}
__attribute__((constructor)) static void initialize(void) {
    int saved = errno;
    pthread_once(&configured, configure);
    errno = saved;
}
static size_t private_confstr(int name, char *buffer, size_t length) {
    if (name != _CS_DARWIN_USER_TEMP_DIR) return confstr(name, buffer, length);
    int saved = errno;
    pthread_once(&configured, configure);
    errno = saved;
    if (!temporary) return confstr(name, buffer, length);
    if (buffer && length) {
        size_t count = temporary_size < length ? temporary_size : length;
        memcpy(buffer, temporary, count - 1);
        buffer[count - 1] = 0;
    }
    return temporary_size;
}
/* dyld excludes this image's own references from its interposition, so the
 * fallback calls the original libc entry rather than recursing. This library
 * is inserted only into the isolated Android process, never the launcher. */
__attribute__((used, section("__DATA,__interpose")))
static const struct { const void *replacement, *original; } mapping = {
    (const void *)private_confstr, (const void *)confstr
};
