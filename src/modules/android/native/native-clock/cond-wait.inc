struct klj_cond_cleanup { cnd_entry *condition; mtx_entry *mutex; int depth; };
static void klj_cond_cancelled(void *opaque) {
    struct klj_cond_cleanup *state = opaque;
    atomic_fetch_sub(&state->condition->waiters, 1);
    cnd_sleep_leave();
    /* Darwin reacquires the mutex before cancellation cleanup. */
    mtx_wait_resume(state->mutex, state->depth, 1, NULL);
    mtx_entry_release(state->mutex);
}
static int klj_cond_clockwait(void *c, void *m, int clock, const struct timespec *deadline) {
    if (!klj_cond_valid(c) || !m || !klj_nsec_valid(deadline) || (clock != 0 && clock != 1)) return EINVAL;
    if (atomic_load((_Atomic(uint32_t) *)c) & 1) return px(ENOTSUP);
    /* The ordinary Darwin entry remains a cancellation point. It immediately
     * translates this fresh realtime deadline to a relative kernel wait.
     * relative_np itself is explicitly NOCANCEL in Apple's implementation. */
    struct timespec target = clock == 0 ? *deadline : klj_realtime_after(klj_clock_remaining(1, deadline));
    mtx_entry *e = mtx_entry_for(m);
    if (!e) return px(errno);
    int depth = mtx_wait_suspend(e);
    cnd_sleep_enter(c, m, __builtin_return_address(0));
    cnd_entry *ce = cnd_entry_for(c);
    cnd_watchdog_start();
    atomic_store(&ce->waiter_ra, __builtin_return_address(0));
    atomic_store(&ce->waiter_ra2, kl_caller_ra2(__builtin_frame_address(0)));
    atomic_fetch_add(&ce->waiters, 1);
    int raw;
    struct klj_cond_cleanup cleanup = {ce, e, depth};
    pthread_cleanup_push(klj_cond_cancelled, &cleanup);
    raw = pthread_cond_timedwait(cnd_host(ce), mtx_host(e), &target);
    pthread_cleanup_pop(0);
    atomic_fetch_sub(&ce->waiters, 1);
    cnd_sleep_leave();
    if (raw != ETIMEDOUT) cnd_wait_failed(raw, c, m, __builtin_return_address(0), NULL);
    mtx_wait_resume(e, depth, raw == 0 || raw == ETIMEDOUT, __builtin_return_address(0));
    mtx_entry_release(e);
    return px(raw);
}
int klb_pthread_cond_timedwait(void *c, void *m, const struct timespec *deadline) {
    if (!klj_cond_valid(c)) return EINVAL;
    return klj_cond_clockwait(c, m, (atomic_load((_Atomic(uint32_t) *)c) >> 1) & 1, deadline);
}
int klb_pthread_cond_clockwait(void *c, void *m, int clock, const struct timespec *deadline) {
    return klj_cond_clockwait(c, m, clock, deadline);
}
int klb_pthread_cond_timedwait_monotonic_np(void *c, void *m, const struct timespec *deadline) {
    return klj_cond_clockwait(c, m, 1, deadline);
}
