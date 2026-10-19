/** Keep nativehelper's complete descriptor helpers and cache accessors while
 * resolving metadata against the JVM that owns the Java objects. */
export function adaptConstantsSource(source){
  const replace=(before,after)=>{
    if(source.split(before).length!==2)throw Error('Original nativehelper metadata boundary changed: '+before);
    source=source.replace(before,after);
  };
  replace('V(FileDescriptor, setInt$, "setInt$", "(I)V", false)',
    'V(FileDescriptor, setInt$, "set", "(I)V", false)');
  replace('//   <Class, field, signature, is_static>',
    '//   <Class, field, JVM field name, signature, is_static>');
  for(const [field,type]of [['descriptor','I'],['_elementSizeShift','I'],['address','J'],['limit','I'],['position','I']]){
    const cls=field==='descriptor'?'FileDescriptor':'NioBuffer';
    replace(`V(${cls}, ${field}, "${type}", false)`,
      `V(${cls}, ${field}, "${field==='descriptor'?'fd':field}", "${type}", false)`);
  }
  for(const [kind,name]of [['jclass','CLASS_NAME(cls)'],['jmethodID','METHOD_NAME(cls, method)'],['jfieldID','FIELD_NAME(cls, field)']])
    replace(`static ${kind} ${name} = NULL;`,
      `static ${kind} ${name} = NULL; \\\n  static pthread_once_t ONCE_NAME(${name}) = PTHREAD_ONCE_INIT;`);
  replace('// Declare storage for cached classes, methods and fields.',
    '// Declare storage for cached classes, methods and fields.\n// Each accessor resolves only its own metadata; Android-only buffer layouts\n// are supplied by JNIPlatformHelp at the JVM representation boundary.');
  replace('#define CLASS_NAME(cls)',
    '#define ONCE_NAME_INNER(name) name ## _once\n#define ONCE_NAME(name) ONCE_NAME_INNER(name)\n\n#define CLASS_NAME(cls)');
  replace('    return (*env)->NewGlobalRef(env, cls);',
    '    jclass global = (*env)->NewGlobalRef(env, cls);\n    (*env)->DeleteLocalRef(env, cls);\n    return global;');
  const start=source.indexOf('static pthread_once_t g_initialized = PTHREAD_ONCE_INIT;');
  const end=source.indexOf('// API exported by libnativehelper_api.h.',start);
  if(start<0||end<start)throw Error('Original nativehelper initialization boundary changed');
  source=source.slice(0,start)+String.raw`// pthread_once publishes each cache to all threads. JNI environments belong
// to individual threads, including simultaneous first uses of separate caches.
static _Thread_local JNIEnv* g_init_env;

#define JCLASS_INITIALIZE(cls, signature, androidOnly)                       \
static void Initialize_ ## cls() {                                         \
    CLASS_NAME(cls) = FindClass(g_init_env, signature, androidOnly);          \
}
JCLASS_CONSTANTS_LIST(JCLASS_INITIALIZE)
#undef JCLASS_INITIALIZE

#define JMETHODID_INITIALIZE(cls, method, name, signature, isStatic)          \
static void Initialize_ ## cls ## _ ## method() {                           \
    jclass type = JniConstants_ ## cls ## Class(g_init_env);                 \
    if (type != NULL) METHOD_NAME(cls, method) =                            \
        FindMethod(g_init_env, type, name, signature, isStatic);             \
}
JMETHODID_CONSTANTS_LIST(JMETHODID_INITIALIZE)
#undef JMETHODID_INITIALIZE

#define JFIELDID_INITIALIZE(cls, field, name, signature, isStatic)            \
static void Initialize_ ## cls ## _ ## field() {                            \
    jclass type = JniConstants_ ## cls ## Class(g_init_env);                 \
    if (type != NULL) FIELD_NAME(cls, field) =                              \
        FindField(g_init_env, type, name, signature, isStatic);              \
}
JFIELDID_CONSTANTS_LIST(JFIELDID_INITIALIZE)
#undef JFIELDID_INITIALIZE

extern void klj_jvm_nio_reset(void);

`+source.slice(end);
  replace('void jniUninitializeConstants() {',
    'void jniUninitializeConstants() {\n    klj_jvm_nio_reset();\n    pthread_once_t o = PTHREAD_ONCE_INIT;');
  for(const [macro,args,name]of [
    ['JCLASS_INVALIDATE','cls, ...','CLASS_NAME(cls)'],
    ['JMETHODID_INVALIDATE','cls, method, ...','METHOD_NAME(cls, method)'],
    ['JFIELDID_INVALIDATE','cls, field, ...','FIELD_NAME(cls, field)'],
  ])replace(`#define ${macro}(${args}) ${name} = NULL;`,
    `#define ${macro}(${args}) ${name} = NULL; memcpy(&ONCE_NAME(${name}), &o, sizeof(o));`);
  replace(`    // If jniConstantsUninitialize is called, runtime has shutdown. Reset
    // state as some tests re-start the runtime.
    pthread_once_t o = PTHREAD_ONCE_INIT;
    memcpy(&g_initialized, &o, sizeof(o));`,
    '    // The original owner calls this only after the runtime has shut down.\n    g_init_env = NULL;');
  for(const [name,initializer]of [
    ['CLASS_NAME(cls)','Initialize_ ## cls'],
    ['METHOD_NAME(cls, method)','Initialize_ ## cls ## _ ## method'],
    ['FIELD_NAME(cls, field)','Initialize_ ## cls ## _ ## field'],
  ]){
    const expression=new RegExp('    EnsureInitialized\\(env\\);[^\\n]*\\n(    return '+name.replace(/[()]/g,'\\$&')+';)');
    if(!expression.test(source))throw Error('Original nativehelper accessor changed: '+name);
    source=source.replace(expression,`    g_init_env = env; \\\n    pthread_once(&ONCE_NAME(${name}), ${initializer}); \\\n$1`);
  }
  return source;
}
