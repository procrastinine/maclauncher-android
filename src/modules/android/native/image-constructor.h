#ifndef MACLAUNCHER_IMAGE_CONSTRUCTOR_H
#define MACLAUNCHER_IMAGE_CONSTRUCTOR_H
#include <pthread.h>

/* Embedded in the mapped image; all loader entrances use the same state. */
typedef struct { int state; pthread_t owner; } KlImageInitialization;
enum { KL_IMAGE_NEW, KL_IMAGE_CONSTRUCTING, KL_IMAGE_READY, KL_IMAGE_FAILED };
static pthread_mutex_t klj_image_init_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t klj_image_init_changed=PTHREAD_COND_INITIALIZER;
static void klj_image_aborted(void *opaque) {
    KlImageInitialization *image=(KlImageInitialization *)opaque;
    pthread_mutex_lock(&klj_image_init_lock);
    image->state=KL_IMAGE_FAILED;
    pthread_cond_broadcast(&klj_image_init_changed);
    pthread_mutex_unlock(&klj_image_init_lock);
}
static int klj_image_construct(KlImageInitialization *image,int (*initialize)(void *),void *context) {
    pthread_t self=pthread_self();
#ifndef __ANDROID__
    /* Bionic has no pthread cancellation API. The same state machine still
     * owns recursive and concurrent initialization on both platforms. */
    int cancellation;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&cancellation);
#endif
    pthread_mutex_lock(&klj_image_init_lock);
    while(image->state==KL_IMAGE_CONSTRUCTING && !pthread_equal(image->owner,self))
        pthread_cond_wait(&klj_image_init_changed,&klj_image_init_lock);
    if(image->state!=KL_IMAGE_NEW) {
        int success=image->state!=KL_IMAGE_FAILED;
        pthread_mutex_unlock(&klj_image_init_lock);
#ifndef __ANDROID__
        pthread_setcancelstate(cancellation,NULL);
#endif
        return success;
    }
    image->owner=self;image->state=KL_IMAGE_CONSTRUCTING;
    pthread_mutex_unlock(&klj_image_init_lock);
    int success;
    pthread_cleanup_push(klj_image_aborted,image);
    success=initialize(context);
    pthread_cleanup_pop(0);
    pthread_mutex_lock(&klj_image_init_lock);
    image->state=success?KL_IMAGE_READY:KL_IMAGE_FAILED;
    pthread_cond_broadcast(&klj_image_init_changed);
    pthread_mutex_unlock(&klj_image_init_lock);
#ifndef __ANDROID__
    pthread_setcancelstate(cancellation,NULL);
#endif
    return success;
}
static void klj_image_reject_new(KlImageInitialization *image) {
    pthread_mutex_lock(&klj_image_init_lock);
    if(image->state==KL_IMAGE_NEW)image->state=KL_IMAGE_FAILED;
    pthread_cond_broadcast(&klj_image_init_changed);
    pthread_mutex_unlock(&klj_image_init_lock);
}
static int klj_image_failed(KlImageInitialization *image) {
    pthread_mutex_lock(&klj_image_init_lock);
    int failed=image->state==KL_IMAGE_FAILED;
    pthread_mutex_unlock(&klj_image_init_lock);
    return failed;
}
#endif
