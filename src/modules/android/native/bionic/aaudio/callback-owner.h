#ifndef MACLAUNCHER_AAUDIO_CALLBACK_OWNER_H
#define MACLAUNCHER_AAUDIO_CALLBACK_OWNER_H
#include <stdint.h>
/* Native callback addresses remain in their execution domain. The backend
 * freezes this plain descriptor in each stream before it can start a feeder. */
typedef struct {
    uintptr_t function,user,caller;
} KljAAudioCallback;
enum {KLJ_AAUDIO_CALLBACK_DATA=1,KLJ_AAUDIO_CALLBACK_ERROR=2,KLJ_AAUDIO_CALLBACK_PRESENTATION_END=3};
/* Install uses the original builder setter. Opening a stream copies its
 * descriptor and rebinds native userdata to that stream's embedded copy;
 * builder deletion and stream close then retain their original ownership. */
typedef void (*KljAAudioCallbackInstall)(void *builder,unsigned kind,void *nativeCallback,const KljAAudioCallback *callback);
#endif
