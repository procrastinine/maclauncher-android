/* Darwin reports inode changes through kqueue but has no open/close filter.
 * Real operations from the JVM and guest libc enter the same upstream worker
 * queue, keeping watch identity, ordering, overflow and retirement in one owner. */
static atomic_uint host_interest[32];
void klj_file_event_interest_change(uint32_t before, uint32_t after) {
    const uint32_t supported = IN_OPEN | IN_ACCESS | IN_CLOSE;
    for (unsigned i = 0; i < 32; ++i) {
        uint32_t bit = 1u << i;
        if (!(bit & supported) || !((before ^ after) & bit)) continue;
        if (after & bit) atomic_fetch_add(&host_interest[i], 1);
        else atomic_fetch_sub(&host_interest[i], 1);
    }
}
int klj_file_event_interest(uint32_t mask) {
    for (unsigned i = 0; i < 32; ++i)
        if ((mask & (1u << i)) && atomic_load_explicit(&host_interest[i], memory_order_relaxed)) return 1;
    return 0;
}
void klj_file_event_publish(const klj_file_event *event) {
    if (!event->mask || !klj_file_event_interest(event->mask)) return;
    int saved = errno;
    workerset_rlock();
    unsigned count = atomic_load(&nworkers), used = 0;
    struct worker **list = calloc(count ? count : 1, sizeof(*list)), *worker;
    if (list) SLIST_FOREACH(worker, &workers, next) {
        worker_ref(worker); list[used++] = worker;
    }
    workerset_unlock();
    for (unsigned i = 0; i < used; ++i) {
        worker = list[i]; worker_cmd_lock(worker);
        if (worker->io[INOTIFY_FD] >= 0) {
            struct worker_cmd command = {.type=WCMD_HOST_EVENT, .retval=-1, .error=EBADF};
            command.cmd.host_event = event;
            if (worker_notify(worker, &command) >= 0) worker_wait(worker);
        }
        worker_cmd_unlock(worker); worker_unref(worker);
    }
    free(list); errno = saved;
}
