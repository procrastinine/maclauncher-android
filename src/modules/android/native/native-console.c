#include "klepton.h"
#include "native-invocation.h"
#include <stdio.h>

/* The running Android application owns its engine and command buffer. Resolve
 * that engine's existing export without constructing another Activity or VM. */
void kl_xash_console_cmd(const char *command){
    if(!command||!*command)return;
    void (*append)(const char *)=(void (*)(const char *))kl_guest_sym_global("Cbuf_AddText");
    if(!append)return;
    KLJ_INVOKE_GUEST(klj_guest_thread_environment(),append(command));
    fprintf(stderr,"  [xash] console: %s",command);
}
