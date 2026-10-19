/* Process-local property owner, included exactly once by legacy-cxx-shim.c.
 * Original host computed defaults and Android API identity enter this same area.
 * This is not Android init's cross-process or persistent property service. */
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <limits.h>

struct klj_property_entry {
    char *name;
    char value[92];
    uint32_t serial;
    bool readonly, written, present;
    struct klj_property_entry *next;
};
static pthread_mutex_t klj_property_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t klj_property_condition = PTHREAD_COND_INITIALIZER;
static struct klj_property_entry *klj_property_entries;
static uint32_t klj_property_area_generation = 1;

static struct klj_property_entry *klj_property_named_locked(const char *name) {
    if (name) for (struct klj_property_entry *p=klj_property_entries;p;p=p->next)
        if (!strcmp(p->name,name)) return p;
    return NULL;
}
static struct klj_property_entry *klj_property_handle_locked(const void *handle) {
    for (struct klj_property_entry *p=klj_property_entries;p;p=p->next) if (p==handle) return p;
    return NULL;
}
static void klj_property_changed_locked(struct klj_property_entry *p) {
    p->serial=((uint32_t)strlen(p->value)<<24)|((p->serial+2)&0x00fffffe);
    ++klj_property_area_generation;
    pthread_cond_broadcast(&klj_property_condition);
}
static bool klj_property_default_locked(const char *name,char value[92]) {
    const struct klj_android_property *fixed=klj_android_property_named(name);
    if (fixed) { snprintf(value,92,"%s",fixed->value); return true; }
    const void *(*find)(const char *)=kl_upstream_shim_lookup("__system_property_find");
    int (*get)(const char *,char *)=kl_upstream_shim_lookup("__system_property_get");
    if (!find||!get||!find(name)) { value[0]=0; return false; }
    get(name,value);value[91]=0;return true;
}
static struct klj_property_entry *klj_property_create_locked(const char *name,const char *value,bool written) {
    struct klj_property_entry *p=calloc(1,sizeof *p);
    if (!p) return NULL;
    p->name=strdup(name);if(!p->name){free(p);return NULL;}
    snprintf(p->value,sizeof p->value,"%s",value);
    p->readonly=!strncmp(name,"ro.",3);p->written=written;p->present=true;
    p->next=klj_property_entries;klj_property_entries=p;klj_property_changed_locked(p);return p;
}
static void klj_property_update_default_locked(struct klj_property_entry *p) {
    if (p->written||p->readonly) return;
    char value[92];bool present=klj_property_default_locked(p->name,value);
    if (p->present!=present||strcmp(value,p->value)) {
        memcpy(p->value,value,strlen(value)+1);p->present=present;klj_property_changed_locked(p);
    }
}
static struct klj_property_entry *klj_property_find_locked(const char *name) {
    if (!name||!*name) return NULL;
    struct klj_property_entry *p=klj_property_named_locked(name);
    if (p) { klj_property_update_default_locked(p); return p->present?p:NULL; }
    char value[92];if(!klj_property_default_locked(name,value))return NULL;
    return klj_property_create_locked(name,value,false);
}
extern int klb_sysprop_foreach(void (*callback)(const void *,void *),void *cookie);
static void klj_property_import_locked(const void *handle,void *cookie) {
    (void)cookie; /* Original Klepton's stable opaque handles are property names. */
    if (handle) klj_property_find_locked((const char *)handle);
}
static uint32_t klj_property_refresh_locked(void) {
    for(size_t i=0;i<sizeof klj_android_properties/sizeof *klj_android_properties;i++)
        klj_property_find_locked(klj_android_properties[i].name);
    klb_sysprop_foreach(klj_property_import_locked,NULL);
    for(struct klj_property_entry *p=klj_property_entries;p;p=p->next)klj_property_update_default_locked(p);
    return klj_property_area_generation;
}
static int klj_android_property_get(const char *name,char *value) {
    pthread_mutex_lock(&klj_property_mutex);
    struct klj_property_entry *p=klj_property_find_locked(name);
    int length=p?(int)strlen(p->value):0;
    if(value){if(p)memcpy(value,p->value,(size_t)length+1);else value[0]=0;}
    pthread_mutex_unlock(&klj_property_mutex);return length;
}
static const void *klj_android_property_find(const char *name) {
    pthread_mutex_lock(&klj_property_mutex);struct klj_property_entry *p=klj_property_find_locked(name);
    pthread_mutex_unlock(&klj_property_mutex);return p;
}
static int klj_android_property_read(const void *handle,char *name,char *value) {
    pthread_mutex_lock(&klj_property_mutex);struct klj_property_entry *p=klj_property_handle_locked(handle);
    if(p)klj_property_update_default_locked(p);
    int length=p?(int)strlen(p->value):0;
    if(name)snprintf(name,32,"%s",p?p->name:"");
    if(value){if(p)memcpy(value,p->value,(size_t)length+1);else value[0]=0;}
    pthread_mutex_unlock(&klj_property_mutex);return length;
}
static void klj_android_property_read_callback(const void *handle,
        void (*callback)(void *,const char *,const char *,uint32_t),void *cookie) {
    if(!callback){errno=EINVAL;return;}
    char copied[92];const char *name=NULL,*value=NULL;uint32_t serial=0;
    pthread_mutex_lock(&klj_property_mutex);struct klj_property_entry *p=klj_property_handle_locked(handle);
    if(p){klj_property_update_default_locked(p);name=p->name;serial=p->serial;
        if(p->readonly)value=p->value;else{memcpy(copied,p->value,strlen(p->value)+1);value=copied;}}
    pthread_mutex_unlock(&klj_property_mutex);
    /* Callback can reenter property APIs. Only read-only pointers are retained. */
    if(value)callback(cookie,name,value,serial);else errno=EINVAL;
}
static uint32_t klj_android_property_serial(const void *handle) {
    pthread_mutex_lock(&klj_property_mutex);struct klj_property_entry *p=klj_property_handle_locked(handle);
    if(p)klj_property_update_default_locked(p);uint32_t serial=p?p->serial:UINT32_MAX;
    pthread_mutex_unlock(&klj_property_mutex);return serial;
}
static uint32_t klj_android_property_area_serial(void) {
    pthread_mutex_lock(&klj_property_mutex);uint32_t serial=klj_property_refresh_locked();
    pthread_mutex_unlock(&klj_property_mutex);return serial;
}
static int klj_android_property_foreach(void (*callback)(const void *,void *),void *cookie) {
    if(!callback){errno=EINVAL;return -1;}
    pthread_mutex_lock(&klj_property_mutex);klj_property_refresh_locked();size_t count=0;
    for(struct klj_property_entry *p=klj_property_entries;p;p=p->next)if(p->present)count++;
    struct klj_property_entry **snapshot=malloc(count*sizeof *snapshot);
    if(!snapshot&&count){pthread_mutex_unlock(&klj_property_mutex);errno=ENOMEM;return -1;}
    size_t i=0;for(struct klj_property_entry *p=klj_property_entries;p;p=p->next)if(p->present)snapshot[i++]=p;
    pthread_mutex_unlock(&klj_property_mutex);
    for(i=0;i<count;i++)callback(snapshot[i],cookie);free(snapshot);return 0;
}
struct klj_property_index {unsigned requested,current;const void *found;};
static void klj_android_property_at(const void *handle,void *cookie) {
    struct klj_property_index *index=cookie;if(index->current++==index->requested)index->found=handle;
}
static const void *klj_android_property_find_nth(unsigned requested) {
    struct klj_property_index index={requested,0,NULL};klj_android_property_foreach(klj_android_property_at,&index);return index.found;
}
static bool klj_property_valid_name(const char *name) {
    if(!name||!*name||name[0]=='.')return false;
    char previous=0;
    for(const unsigned char *p=(const unsigned char *)name;*p;previous=*p,p++) {
        if(*p=='.'&&previous=='.')return false;
        /* Original init/util.cpp IsLegalPropertyName includes @ and :. */
        if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='.'||*p=='_'||*p=='-'||*p=='@'||*p==':'))return false;
    }
    return previous!='.';
}
static bool klj_property_valid_utf8(const char *value) {
    /* init validates UTF-8 in its UTF-8 locale. Do not depend on a guest's
     * mutable process locale at this native boundary. */
    const unsigned char *p=(const unsigned char *)value;
    while(*p){
        unsigned first=*p++,code;int extra;
        if(first<0x80)continue;
        if(first>=0xc2&&first<=0xdf){code=first&31;extra=1;}
        else if(first>=0xe0&&first<=0xef){code=first&15;extra=2;}
        else if(first>=0xf0&&first<=0xf4){code=first&7;extra=3;}
        else return false;
        for(int i=0;i<extra;i++){unsigned next=*p;if(next<0x80||next>0xbf)return false;p++;code=(code<<6)|(next&63);}
        if((extra==2&&code<0x800)||(extra==3&&code<0x10000)||(code>=0xd800&&code<=0xdfff)||code>0x10ffff)return false;
    }
    return true;
}
static int klj_android_property_set(const char *name,const char *value) {
    if(!klj_property_valid_name(name)){errno=EINVAL;return -1;}
    if(!strncmp(name,"ro.",3)){errno=EROFS;return -1;}
    if(!value)value="";
    if(strlen(value)>=92){errno=E2BIG;return -1;}
    if(!klj_property_valid_utf8(value)){errno=EILSEQ;return -1;}
    pthread_mutex_lock(&klj_property_mutex);struct klj_property_entry *p=klj_property_named_locked(name);
    if(p){memcpy(p->value,value,strlen(value)+1);p->written=true;p->present=true;klj_property_changed_locked(p);}
    else p=klj_property_create_locked(name,value,true);
    pthread_mutex_unlock(&klj_property_mutex);return p?0:-1;
}
static long double klj_property_monotonic_seconds(void) {
    struct timespec time;clock_gettime(CLOCK_MONOTONIC,&time);
    return (long double)time.tv_sec+(long double)time.tv_nsec/1000000000.0L;
}
static bool klj_android_property_wait(const void *handle,uint32_t previous,uint32_t *next,const struct timespec *timeout) {
    if(!next||(timeout&&(timeout->tv_sec<0||timeout->tv_nsec<0||timeout->tv_nsec>=1000000000))){errno=EINVAL;return false;}
    const long double deadline=timeout?klj_property_monotonic_seconds()+(long double)timeout->tv_sec+(long double)timeout->tv_nsec/1000000000.0L:0;
    pthread_mutex_lock(&klj_property_mutex);
    for(;;){
        struct klj_property_entry *p=handle?klj_property_handle_locked(handle):NULL;
        if(handle&&!p){pthread_mutex_unlock(&klj_property_mutex);errno=EINVAL;return false;}
        if(p)klj_property_update_default_locked(p);
        uint32_t serial=p?p->serial:klj_property_refresh_locked();
        if(serial!=previous){*next=serial;pthread_mutex_unlock(&klj_property_mutex);return true;}
        long double remaining=timeout?deadline-klj_property_monotonic_seconds():0;
        if(timeout&&remaining<=0){pthread_mutex_unlock(&klj_property_mutex);errno=ETIMEDOUT;return false;}
        bool computed=!p||(!p->written&&!p->readonly);
        if(!computed&&!timeout){pthread_cond_wait(&klj_property_condition,&klj_property_mutex);continue;}
        /* Poll only while an actual caller waits for a computed host value. */
        if(computed&&(!timeout||remaining>0.01L))remaining=0.01L;
        if(remaining>86400.0L)remaining=86400.0L;
        struct timespec interval={(time_t)remaining,(long)((remaining-(time_t)remaining)*1000000000.0L)};
        pthread_cond_timedwait_relative_np(&klj_property_condition,&klj_property_mutex,&interval);
    }
}
static uint32_t klj_android_property_wait_any(uint32_t previous) {
    uint32_t next=previous;klj_android_property_wait(NULL,previous,&next,NULL);return next;
}
