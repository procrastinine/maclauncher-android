#pragma once
#include <poll.h>

/* The descriptor layout agrees; the event words belong to Linux. */
int klj_poll(struct pollfd *descriptors, nfds_t count, int timeout);
