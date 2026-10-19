import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
/** Reuse Klepton's store/editor/XML implementation for distinct storage roots. */
export function preparePreferencesSource(klepton, build) {
  let text=fs.readFileSync(path.join(klepton,'runtime/jni/kl_jni_prefs.c'),'utf8');
  const replace=(from,to)=>{if(text.split(from).length!==2)throw Error('Pinned Klepton preferences source changed: '+from.slice(0,80));text=text.replace(from,to);};
  replace('// in. Set<String> is absent because nothing has asked for it; the primitive\n// types are all one shape, so they come as a group.','// in. The owned extension adds bounded string sets in the same XML store.');
  replace('{"string", \'S\'}, {"int", \'I\'}','{"set", \'T\'}, {"string", \'S\'}, {"int", \'I\'}');
  replace('        if (e->kind == \'S\') {',`        if (e->kind == 'T') {
            fputs("    <set name=\\\"",f);klj_xml_escape(f,e->key);fputs("\\\">",f);
            fputs(e->sval?e->sval:"",f);fputs("</set>\\n",f);continue;
        }
        if (e->kind == 'S') {`);
  replace('    if (sz < 0) { fclose(f); return; }','    if (sz < 0 || sz > 32*1024*1024) { fclose(f); return; }');
  replace('    size_t len = fread(buf, 1, (size_t)sz, f);','    if (!buf) { fclose(f); return; }\n    size_t len = fread(buf, 1, (size_t)sz, f);');
  replace('        if (kind == \'S\') {',`        if (kind == 'T') {
            char *end=close[-1]=='/'?close:strstr(close+1,"</set>");
            size_t bytes=end?(end==close?0:(size_t)(end-close-1)):0;
            if(end && bytes<=16u*1024u*1024u && p->kv.n<65536u){
                char *value=malloc(bytes+1);
                if(value){memcpy(value,close+1,bytes);value[bytes]=0;klj_pref *e=klj_pref_slot(&p->kv,key);e->kind='T';e->sval=value;}
            }
            free(key);q=end?(end==close?close:end+5):close;continue;
        }
        if (kind == 'S') {`);
  // Keep the last valid file on failed writes and report commit failure.
  replace('static void klj_prefs_save(klj_prefs *p) {\n    FILE *f = fopen(p->path, "wb");',`static int klj_prefs_save(klj_prefs *p) {
    if(p->kv.n>65536u)return 0;
    char temporary[sizeof p->path+8];snprintf(temporary,sizeof temporary,"%s.tmp",p->path);
    FILE *f = fopen(temporary, "wb");`);
  replace('if (!f) { KLJ_LOG("SharedPreferences[%s]: cannot write %s", p->name, p->path); return; }','if (!f) { KLJ_LOG("SharedPreferences[%s]: cannot write %s", p->name, p->path); return 0; }');
  replace('    fputs("</map>\\n", f);\n    fclose(f);',`    fputs("</map>\\n", f);
    long size=ftell(f);
    int ok=size>=0 && size<=32*1024*1024 && !ferror(f) && fflush(f)==0 && fsync(fileno(f))==0;
    if(fclose(f)!=0)ok=0;
    if(ok && rename(temporary,p->path)!=0)ok=0;
    if(!ok)unlink(temporary);
    return ok;`);
  replace('static void klj_editor_flush(void *self) {','static int klj_editor_flush(void *self) {');
  replace('    if (!ed || !ed->store) return;','    if (!ed || !ed->store) return 0;');
  replace('    klj_prefs_save(p);','    int saved=klj_prefs_save(p);');
  replace('    ed->clear = 0;\n}', '    ed->clear = 0;\n    return saved;\n}');
  replace('    klj_editor_flush(self);\n    return (klj_val){.j = 1};','    return (klj_val){.j = klj_editor_flush(self)};');
  const helper=path.join(path.dirname(fileURLToPath(import.meta.url)),'preferences-set.h');
  replace('// Every Editor mutator returns `this`', '#include '+JSON.stringify(helper)+'\n\n// Every Editor mutator returns `this`');
  replace('#define KLJ_MAX_PREF_FILES 8','#define KLJ_MAX_PREF_FILES 128\nstatic _Thread_local const char *klj_host_preferences_directory;\nstatic pthread_mutex_t klj_host_preferences_lock=PTHREAD_MUTEX_INITIALIZER;');
  replace('static klj_val klj_Context_getSharedPreferences(void *env, void *self, const klj_val *a, int n) {','static klj_val klj_Context_getSharedPreferences_unlocked(void *env, void *self, const klj_val *a, int n) {');
  replace('    if (!name) name = "default";','    if (!name) name = "default";\n    char dir[1024], target[1024];\n    int length=klj_host_preferences_directory?snprintf(dir,sizeof dir,"%s",klj_host_preferences_directory):snprintf(dir,sizeof dir,"%s/shared_prefs",kl_jni_files_dir());\n    if(length<0||(size_t)length>=sizeof dir||strlen(name)>=128||strchr(name,\'/\')||strchr(name,\'\\\\\'))return (klj_val){.l=NULL};\n    length=snprintf(target,sizeof target,"%s/%s.xml",dir,name);\n    if(length<0||(size_t)length>=sizeof target)return (klj_val){.l=NULL};');
  replace('strcmp(g_prefs_files[i].name, name) == 0','strcmp(g_prefs_files[i].path, target) == 0');
  replace('    char dir[1024];\n    snprintf(dir, sizeof dir, "%s/shared_prefs", kl_jni_files_dir());\n    klj_mkdir_p(dir);\n    snprintf(p->path, sizeof p->path, "%s/%s.xml", dir, name);','    klj_mkdir_p(dir);\n    memcpy(p->path,target,(size_t)length+1);');
  const anchor='static klj_prefs *klj_prefs_of(void *self)';
  replace(anchor,`static klj_val klj_Context_getSharedPreferences(void *env,void *self,const klj_val *a,int n) {
    pthread_mutex_lock(&klj_host_preferences_lock);
    klj_val result=klj_Context_getSharedPreferences_unlocked(env,self,a,n);
    pthread_mutex_unlock(&klj_host_preferences_lock);
    return result;
}
void *klj_host_preferences_at(const char *directory,const char *name) {
    const char *previous=klj_host_preferences_directory;
    klj_host_preferences_directory=directory;
    klj_val args[2]={{.l=kl_jni_new_string(name)},{.j=0}};
    void *result=klj_Context_getSharedPreferences(kl_jni_env(),NULL,args,2).l;
    klj_host_preferences_directory=previous;
    return result;
}

`+anchor);
  const result=path.join(build,'kl_jni_prefs.c');fs.writeFileSync(result,text);return result;
}
