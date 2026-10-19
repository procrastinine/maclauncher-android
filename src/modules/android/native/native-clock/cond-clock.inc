/* Bionic LP64 condattr is a long; state bit 1 chooses MONOTONIC. Static
 * pthread_cond_t initializers use the same first 32-bit flags. The existing
 * address-keyed host condition table remains bounded and unchanged. */
static int klj_cond_valid(void *c) {
    return c && !(atomic_load((_Atomic(uint32_t) *)c) & ~3u);
}
int klb_pthread_cond_init(void *c, const void *attr) {
    if (!c || (attr && (*(const long *)attr & ~3L))) return EINVAL;
    long flags = attr ? *(const long *)attr : 0;
    if (flags & 1) return px(ENOTSUP); /* This table is process-local. */
    cnd_entry *ce = cnd_entry_for(c);
    if (atomic_load(&ce->waiters)) return EBUSY;
    cnd_host(ce);
    atomic_store((_Atomic(uint32_t) *)c, (uint32_t)flags);
    return 0;
}
int klb_pthread_cond_destroy(void *c) {
    if (!klj_cond_valid(c)) return EINVAL;
    if (atomic_load(&cnd_entry_for(c)->waiters)) return EBUSY;
    atomic_store((_Atomic(uint32_t) *)c, 0xdeadc04du);
    return 0;
}
int klb_pthread_condattr_init(long *attr) { if (!attr) return EINVAL; *attr = 0; return 0; }
int klb_pthread_condattr_destroy(long *attr) {
    if (!attr || (*attr & ~3L)) return EINVAL;
    *attr = 0xdeada11dL; return 0;
}
int klb_pthread_condattr_setclock(long *attr, int clock) {
    if (!attr || (*attr & ~3L) || (clock != 0 && clock != 1)) return EINVAL;
    *attr = (*attr & ~2L) | ((long)clock << 1); return 0;
}
int klb_pthread_condattr_getclock(const long *attr, int *clock) {
    if (!attr || !clock || (*attr & ~3L)) return EINVAL;
    *clock = (int)((*attr >> 1) & 1); return 0;
}
int klb_pthread_condattr_setpshared(long *attr, int shared) {
    if (!attr || (*attr & ~3L) || (shared != 0 && shared != 1)) return EINVAL;
    *attr = (*attr & ~1L) | shared; return 0;
}
int klb_pthread_condattr_getpshared(const long *attr, int *shared) {
    if (!attr || !shared || (*attr & ~3L)) return EINVAL;
    *shared = (int)(*attr & 1); return 0;
}
