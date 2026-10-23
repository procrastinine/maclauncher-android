/* Original NDK-produced ELF metadata; this Java contract control executes no ELF. */
#ifdef DEPENDENCY
long dependency(void) { return 7; }
#else
#ifdef NEED_DEPENDENCY
extern long dependency(void);
#else
static long dependency(void) { return 7; }
#endif
long Java_fixture_Missing_ping(void *environment, void *target) {
    (void)environment; (void)target;
    return dependency();
}
#endif
