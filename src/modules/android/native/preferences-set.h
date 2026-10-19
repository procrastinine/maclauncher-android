// Included after the pinned Klepton preference/editor definitions. A string set
// uses the existing owned sval slot; its XML children are persisted by the same
// file writer and copied/released by the existing editor/snapshot lifecycle.
#define KLJ_PREF_SET_BYTES (16u*1024u*1024u)
#define KLJ_PREF_SET_ENTRIES 65536u
const char *klj_host_preferences_string_set(void *self,const char *key,int *status) {
    klj_object *object=klj_as_object(self);
    if(!object||strcmp(object->cls,"android/content/SharedPreferences")||!object->data){*status=EINVAL;return NULL;}
    klj_prefs *store=object->data;
    klj_pref *entry=klj_pref_find(&store->kv,key);
    if(!entry){*status=0;return NULL;}
    if(entry->kind!='T'){*status=EDOM;return NULL;}
    *status=0;return entry->sval?entry->sval:"";
}
int klj_host_editor_string_set(void *self,const char *key,const char *xml) {
    klj_object *object=klj_as_object(self);
    if(!object||strcmp(object->cls,"android/content/SharedPreferences$Editor")||!object->data)return EINVAL;
    klj_editor *editor=object->data;
    if(!editor->store||strlen(key)>KLJ_PREF_SET_BYTES)return EINVAL;
    if(xml&&(strnlen(xml,KLJ_PREF_SET_BYTES+1)>KLJ_PREF_SET_BYTES||strstr(xml,"</set")))return EINVAL;
    char *copy=xml?strdup(xml):NULL;
    if(xml&&!copy)return ENOMEM;
    klj_pref *entry=klj_pref_find(&editor->pending,key);
    if(!entry){
        if(editor->pending.n>=KLJ_PREF_SET_ENTRIES){free(copy);return E2BIG;}
        char *name=strdup(key);if(!name){free(copy);return ENOMEM;}
        if(editor->pending.n==editor->pending.cap){
            unsigned capacity=editor->pending.cap?editor->pending.cap*2:16;
            klj_pref *next=realloc(editor->pending.v,capacity*sizeof *next);
            if(!next){free(name);free(copy);return ENOMEM;}
            editor->pending.v=next;editor->pending.cap=capacity;
        }
        entry=&editor->pending.v[editor->pending.n++];memset(entry,0,sizeof *entry);entry->key=name;
    }
    free(entry->sval);entry->sval=copy;entry->kind=xml?'T':'-';entry->ival=0;entry->fval=0;
    return 0;
}
