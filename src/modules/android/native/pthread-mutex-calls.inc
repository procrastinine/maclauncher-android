int klb_pthread_mutex_init(void *guest, const void *attributes) {
    if (!guest) return mtx_null("init");
    int type = attributes ? *(const int *)attributes & 15 : 0;
    if (type > 2) return EINVAL;
    /* Preserve the host address on reinitialization: existing Darwin condition
     * variables can retain that address even when no thread is asleep. */
    mtx_entry *entry = mtx_entry_for_type(guest, type);
    if (!entry) return px(errno);
    if (atomic_load(&entry->depth)) { mtx_entry_release(entry); return EBUSY; }
    atomic_store(&entry->owner, NULL);
    atomic_store(&entry->locksite, NULL);
    atomic_store((_Atomic(uint32_t) *)guest, (uint32_t)type << 14);
    mtx_log("init", guest, mtx_hash((uintptr_t)guest));
    mtx_entry_release(entry);
    return 0;
}
int klb_pthread_mutex_lock(void *guest) {
    if (!guest) return mtx_null("lock");
    mtx_entry *entry = mtx_entry_for(guest);
    if (!entry) return px(errno);
    mtx_wait_enter(guest, __builtin_return_address(0));
    int result = pthread_mutex_lock(mtx_host(entry));
    mtx_wait_leave();
    if (!result) {
        atomic_fetch_add(&entry->depth, 1);
        atomic_store(&entry->locksite, __builtin_return_address(0));
        atomic_store(&entry->owner, (void *)pthread_self());
    }
    mtx_entry_release(entry);
    return px(result);
}
int klb_pthread_mutex_trylock(void *guest) {
    if (!guest) return mtx_null("trylock");
    mtx_entry *entry = mtx_entry_for(guest);
    if (!entry) return px(errno);
    int result = pthread_mutex_trylock(mtx_host(entry));
    if (!result) {
        atomic_fetch_add(&entry->depth, 1);
        atomic_store(&entry->locksite, __builtin_return_address(0));
        atomic_store(&entry->owner, (void *)pthread_self());
    }
    mtx_entry_release(entry);
    return px(result);
}
int klb_pthread_mutex_unlock(void *guest) {
    if (!guest) return mtx_null("unlock");
    mtx_entry *entry = mtx_entry_for(guest);
    if (!entry) return px(errno);
    if (entry->type != 0 && (atomic_load(&entry->owner) != (void *)pthread_self() || atomic_load(&entry->depth) <= 0)) {
        mtx_entry_release(entry); return EPERM;
    }
    int depth = entry->type == 0 ? (atomic_store(&entry->depth, 0), 0) : atomic_fetch_sub(&entry->depth, 1) - 1;
    if (!depth) { atomic_store(&entry->owner, NULL); atomic_store(&entry->locksite, NULL); }
    int result = pthread_mutex_unlock(mtx_host(entry));
    mtx_entry_release(entry);
    return px(result);
}
int klb_pthread_mutex_destroy(void *guest) {
    mtx_log("destroy", guest, 0);
    return px(mtx_destroy(guest));
}
