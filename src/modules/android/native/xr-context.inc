/* Included into the pinned GLFB implementation by xr-sources.mjs. */
static void *(*klj_glfb_resolver)(const char *);
/* Resolver availability is independent of context ownership. GLSurfaceView
 * explicitly initializes Klepton's owned context; NativeSurface/XR borrows the
 * context created by the guest. Both use the same actual ANGLE provider. */
static _Atomic int klj_glfb_owns_context;
static pthread_mutex_t klj_glfb_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t klj_glfb_init_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t klj_interop_lock=PTHREAD_MUTEX_INITIALIZER,klj_device_lock=PTHREAD_MUTEX_INITIALIZER;
static struct {void *context,*display;uint64_t group;unsigned current;int destroyPending;} klj_contexts[128];
static uint64_t klj_next_group=1,klj_projection_group;
static _Thread_local void *klj_current_context,*klj_current_display;
static void *klj_consumer_event,*klj_consumer_sync;
static int klj_consumer_failed;
static pthread_once_t klj_binding_once=PTHREAD_ONCE_INIT;
static pthread_key_t klj_binding_key;
static int klj_projection_current(void);
static void klj_wait_consumer(uint64_t value);

