#include "poll-bridge.h"
#include "pollable-fds/bridge.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static short host_events(unsigned guest) {
    short host = 0;
    if (guest & 0x001) host |= POLLIN;
    if (guest & 0x002) host |= POLLPRI;
    if (guest & 0x004) host |= POLLOUT;
    if (guest & 0x040) host |= POLLRDNORM;
    if (guest & 0x080) host |= POLLRDBAND;
    if (guest & 0x100) host |= POLLWRNORM;
    if (guest & 0x200) host |= POLLWRBAND;
    /* ERR/HUP/NVAL are returned independently of the requested events.
     * Linux ignores reserved request bits; they must not select Darwin's
     * differently numbered vnode events or turn a valid wait into ENOTSUP. */
    return host;
}

static short guest_events(const struct pollfd *host, unsigned requested) {
    unsigned guest = 0;
    if (host->revents & POLLIN) guest |= requested & 0x001;
    if (host->revents & POLLPRI) guest |= requested & 0x002;
    if (host->revents & POLLOUT) guest |= requested & (0x004 | 0x100);
    if (host->revents & POLLRDNORM) guest |= requested & 0x040;
    if (host->revents & POLLRDBAND) guest |= requested & 0x080;
    if (host->revents & POLLWRBAND) guest |= requested & 0x200;
    if (host->revents & POLLERR) guest |= 0x008;
    if (host->revents & POLLHUP) guest |= 0x010;
    if (host->revents & POLLNVAL) guest |= 0x020;
    return (short)guest;
}

static int64_t milliseconds(void) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int klj_poll(struct pollfd *descriptors, nfds_t count, int timeout) {
    if (!count) return klj_pollable_poll(NULL, 0, timeout);
    if (!descriptors) { errno = EFAULT; return -1; }
    int queue = -1;
    for (nfds_t i = 0; i < count; i++) if (descriptors[i].fd >= 0 && (descriptors[i].events & 0x2000)) {
        queue = kqueue();
        if (queue < 0) return -1;
        break;
    }
    size_t total = (size_t)count + (queue >= 0);
    struct pollfd local[65];
    struct pollfd *host = count <= 64 ? local : malloc(total * sizeof(*host));
    if (!host) { if (queue >= 0) klj_pollable_close(queue); errno = ENOMEM; return -1; }
    for (nfds_t i = 0; i < count; i++) {
        host[i] = descriptors[i];
        host[i].events = host_events((unsigned short)descriptors[i].events);
        if (queue >= 0 && descriptors[i].fd >= 0 && (descriptors[i].events & 0x2000)) {
            struct kevent event;
            EV_SET(&event, descriptors[i].fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
            /* Invalid descriptors remain poll's POLLNVAL. Non-socket files
             * have no read-half-close notification. */
            kevent(queue, &event, 1, NULL, 0, NULL);
        }
    }
    if (queue >= 0) host[count] = (struct pollfd){queue, POLLIN, 0};
    int64_t deadline = timeout >= 0 ? milliseconds() + timeout : 0;
    int remaining = timeout, result, error;
    for (;;) {
        result = klj_pollable_poll(host, (nfds_t)total, remaining); error = errno;
        if (result < 0) break;
        result = 0;
        for (nfds_t i = 0; i < count; i++) {
            descriptors[i].revents = guest_events(&host[i], (unsigned short)descriptors[i].events);
        }
        if (queue >= 0 && host[count].revents) {
            struct kevent events[64]; struct timespec immediate = {0}; int found;
            while ((found = kevent(queue, NULL, 0, events, 64, &immediate)) > 0) {
                for (int j = 0; j < found; j++) if (events[j].flags & EV_EOF) {
                    int fd = (int)events[j].ident, type = 0; socklen_t size = sizeof(type);
                    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) || type != SOCK_STREAM) continue;
                    for (nfds_t i = 0; i < count; i++)
                        if (descriptors[i].fd == fd && (descriptors[i].events & 0x2000)) descriptors[i].revents |= 0x2000;
                }
            }
        }
        for (nfds_t i = 0; i < count; i++) result += descriptors[i].revents != 0;
        if (result || queue < 0 || timeout == 0) break;
        if (timeout >= 0) {
            int64_t left = deadline - milliseconds();
            if (left <= 0) break;
            remaining = (int)left;
        }
    }
    if (queue >= 0) klj_pollable_close(queue);
    if (host != local) free(host);
    errno = error;
    return result;
}
