void produce_directory_diff(struct i_watch *,struct kevent *);
static void process_host_event(struct worker *worker, const klj_file_event *event) {
    struct i_watch *watch = SLIST_FIRST(&worker->head);
    while (watch) {
        struct i_watch *next = SLIST_NEXT(watch, next);
        const char *name = NULL;
        bool match = watch->dev == event->file.st_dev && watch->inode == event->file.st_ino;
        if (!match && S_ISDIR(watch->mode) && event->name[0] &&
            watch->dev == event->parent.st_dev && watch->inode == event->parent.st_ino) {
            name = event->name; match = true;
            /* A newly opened child can beat the pending NOTE_WRITE dispatch.
             * Use the original directory diff before publishing its operation
             * so CREATE/arrival precedes OPEN/CLOSE, without polling writes. */
            struct dep_item *known=dl_find(&watch->deps,name);
            if((!known||known->inode!=event->file.st_ino)&&event->file.st_nlink){
                struct kevent change={.fflags=NOTE_WRITE};
                produce_directory_diff(watch,&change);
            }
        }
        uint32_t mask = event->mask & watch->flags & IN_ALL_EVENTS;
        if (match && mask && !watch->is_closed) {
            if (S_ISDIR(event->file.st_mode)) mask |= IN_ISDIR;
            event_queue_enqueue(&worker->eq, watch->wd, mask, 0, name);
            if (watch->flags & IN_ONESHOT) watch->is_closed=true;
        }
        if(watch->is_closed)worker_remove_iwatch(worker,watch);
        watch = next;
    }
}
