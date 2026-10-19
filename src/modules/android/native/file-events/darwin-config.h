/* libinotify-kqueue platform configuration for the macOS 13+ SDK. */
#define BUILD_LIBRARY 1
#define FLEXIBLE_ARRAY_MEMBER
#define HAVE_ATFUNCS 1
#define HAVE_DIRENT_H 1
#define HAVE_FACCESSAT 1
#define HAVE_FDCLOSEDIR 1
#define HAVE_FDOPENDIR 1
#define HAVE_FSTATAT 1
#define HAVE_KEVENT 1
#define HAVE_KQUEUE 1
#define HAVE_OPENAT 1
#define HAVE_PTHREAD 1
#define HAVE_STDATOMIC_H 1
#define HAVE_STDBOOL_H 1
#define HAVE_STDINT_H 1
#define HAVE_STRUCT_DIRENT_D_TYPE 1
#define HAVE_TYPEOF 1
#define HAVE__BOOL 1
/* Duplicate the owned directory descriptor before fdopendir. This uses public
 * APIs on macOS 13 and avoids fdclosedir (only introduced in macOS 26.4). */
#define READDIR_DOES_OPENDIR 1
#define _DARWIN_C_SOURCE 1
