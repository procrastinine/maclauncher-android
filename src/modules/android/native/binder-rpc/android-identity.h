#pragma once
#include <unistd.h>
#include "../process-identity.h"
#include "../process-ids.h"

// Only Android-facing Binder implementation units include this boundary.
// Socket authentication, physical thread keys and host subprocess ownership
// continue to use Darwin credentials in their separate platform units.
#define getpid klj_process_id
#define getuid klj_process_uid
