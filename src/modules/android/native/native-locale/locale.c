#include <errno.h>
#include <locale.h>
#include <string.h>
#include "android-locale.h"

/* This is a record view, not another locale. No strings are allocated, copied,
 * retained or freed here, and uselocale/newlocale keep their physical identity. */
struct klj_android_lconv *klj_localeconv(void){
    const struct lconv *native=localeconv();
    int error=errno;
    static _Thread_local struct klj_android_lconv view;
    memset(&view,0,sizeof(view));
#define COPY(field) view.field=native->field;
    KLJ_LOCALE_FIELDS(COPY)
#undef COPY
    errno=error;
    return &view;
}

void *klj_locale_lookup(const char *name){
    return !strcmp(name,"localeconv")?(void *)klj_localeconv:NULL;
}
