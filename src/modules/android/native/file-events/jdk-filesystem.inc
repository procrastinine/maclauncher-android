/* Keep the JDK's File/stream implementations and descriptors. Only their
 * operating-system filenames enter the existing Android storage namespace. */
static jclass filesystem_paths,filesystem_file;
static jmethodID filesystem_map,filesystem_file_path,filesystem_file_constructor;
static jstring filesystem_path(JNIEnv *env,jstring path){
    jstring mapped=path?(*env)->CallStaticObjectMethod(env,filesystem_paths,filesystem_map,path):NULL;
    if(!mapped||(*env)->ExceptionCheck(env))return mapped;
    const char *name=(*env)->GetStringUTFChars(env,mapped,NULL);if(!name)return mapped;
    char buffer[PATH_MAX];const char *native=filesystem_virtual_path(name)?kl_guest_path(name,buffer,sizeof buffer):name;
    jstring result=mapped;
    if(native!=name){
        /* The owner returns UTF-8 filenames; JNI modified UTF-8 would corrupt
         * a supplementary character in the private cache directory. */
        size_t length=strlen(native);jbyteArray bytes=(*env)->NewByteArray(env,(jsize)length);
        if(bytes){
            (*env)->SetByteArrayRegion(env,bytes,0,(jsize)length,(const jbyte*)native);
            jclass string=(*env)->FindClass(env,"java/lang/String");
            jmethodID constructor=string?(*env)->GetMethodID(env,string,"<init>","([BLjava/lang/String;)V"):NULL;
            jstring charset=constructor?(*env)->NewStringUTF(env,"UTF-8"):NULL;
            if(charset&&!(*env)->ExceptionCheck(env))result=(*env)->NewObject(env,string,constructor,bytes,charset);
            if(charset)(*env)->DeleteLocalRef(env,charset);if(string)(*env)->DeleteLocalRef(env,string);(*env)->DeleteLocalRef(env,bytes);
        }
    }
    (*env)->ReleaseStringUTFChars(env,mapped,name);
    if(result!=mapped)(*env)->DeleteLocalRef(env,mapped);return result;
}
static jobject filesystem_value(JNIEnv *env,jobject file){
    if(!file)return NULL;
    jstring source=(*env)->CallObjectMethod(env,file,filesystem_file_path);
    jstring path=source&&!(*env)->ExceptionCheck(env)?filesystem_path(env,source):NULL;
    jobject result=path&&!(*env)->ExceptionCheck(env)?(*env)->NewObject(env,filesystem_file,filesystem_file_constructor,path):NULL;
    if(path)(*env)->DeleteLocalRef(env,path);if(source)(*env)->DeleteLocalRef(env,source);return result;
}
static int filesystem_initialize(JNIEnv *env){
    if(filesystem_paths&&filesystem_file)return 1;
    jclass paths=(*env)->FindClass(env,"maclauncher/android/framework/HostFilesystem");
    jclass file=paths?(*env)->FindClass(env,"java/io/File"):NULL;
    if(paths&&file){
        filesystem_map=(*env)->GetStaticMethodID(env,paths,"mapPath","(Ljava/lang/String;)Ljava/lang/String;");
        filesystem_file_path=(*env)->GetMethodID(env,file,"getPath","()Ljava/lang/String;");
        filesystem_file_constructor=(*env)->GetMethodID(env,file,"<init>","(Ljava/lang/String;)V");
        if(filesystem_map&&filesystem_file_path&&filesystem_file_constructor){
            filesystem_paths=(*env)->NewGlobalRef(env,paths);filesystem_file=(*env)->NewGlobalRef(env,file);
        }
    }
    if(paths)(*env)->DeleteLocalRef(env,paths);if(file)(*env)->DeleteLocalRef(env,file);
    if(!filesystem_paths||!filesystem_file){
        if(filesystem_paths)(*env)->DeleteGlobalRef(env,filesystem_paths);
        if(filesystem_file)(*env)->DeleteGlobalRef(env,filesystem_file);
        filesystem_paths=filesystem_file=NULL;
    }
    return filesystem_paths&&filesystem_file&&!(*env)->ExceptionCheck(env);
}
#define FILESYSTEM_FILE(NAME,RESULT,EXTRA,ARGS,FAILURE) \
    static RESULT (*filesystem_original_##NAME)(JNIEnv*,jobject,jobject EXTRA); \
    static RESULT filesystem_##NAME(JNIEnv *env,jobject self,jobject file EXTRA){ \
        jobject mapped=filesystem_value(env,file);RESULT result=FAILURE; \
        if(!(*env)->ExceptionCheck(env))result=filesystem_original_##NAME(env,self,mapped ARGS); \
        if(mapped)(*env)->DeleteLocalRef(env,mapped);return result; \
    }
#define FILESYSTEM_COMMA ,
FILESYSTEM_FILE(attributes,jint,,,0)
FILESYSTEM_FILE(access,jboolean,FILESYSTEM_COMMA jint mode,FILESYSTEM_COMMA mode,JNI_FALSE)
FILESYSTEM_FILE(modified,jlong,,,0)
FILESYSTEM_FILE(length,jlong,,,0)
FILESYSTEM_FILE(permission,jboolean,FILESYSTEM_COMMA jint mode FILESYSTEM_COMMA jboolean enabled FILESYSTEM_COMMA jboolean owner,FILESYSTEM_COMMA mode FILESYSTEM_COMMA enabled FILESYSTEM_COMMA owner,JNI_FALSE)
FILESYSTEM_FILE(delete,jboolean,,,JNI_FALSE)
FILESYSTEM_FILE(list,jobjectArray,,,NULL)
FILESYSTEM_FILE(directory,jboolean,,,JNI_FALSE)
FILESYSTEM_FILE(time,jboolean,FILESYSTEM_COMMA jlong time,FILESYSTEM_COMMA time,JNI_FALSE)
FILESYSTEM_FILE(readonly,jboolean,,,JNI_FALSE)
FILESYSTEM_FILE(space,jlong,FILESYSTEM_COMMA jint type,FILESYSTEM_COMMA type,0)
#undef FILESYSTEM_FILE
#undef FILESYSTEM_COMMA
static jboolean (*filesystem_original_rename)(JNIEnv*,jobject,jobject,jobject);
static jboolean filesystem_rename(JNIEnv *env,jobject self,jobject from,jobject to){
    jobject source=filesystem_value(env,from),target=!(*env)->ExceptionCheck(env)?filesystem_value(env,to):NULL;jboolean result=JNI_FALSE;
    if(!(*env)->ExceptionCheck(env))result=filesystem_original_rename(env,self,source,target);
    if(source)(*env)->DeleteLocalRef(env,source);if(target)(*env)->DeleteLocalRef(env,target);return result;
}
#define FILESYSTEM_STRING(NAME,RESULT,FAILURE) \
    static RESULT (*filesystem_original_##NAME)(JNIEnv*,jobject,jstring); \
    static RESULT filesystem_##NAME(JNIEnv *env,jobject self,jstring path){ \
        jstring mapped=filesystem_path(env,path);RESULT result=FAILURE; \
        if(!(*env)->ExceptionCheck(env))result=filesystem_original_##NAME(env,self,mapped); \
        if(mapped)(*env)->DeleteLocalRef(env,mapped);return result; \
    }
FILESYSTEM_STRING(canonical,jstring,NULL)
FILESYSTEM_STRING(create,jboolean,JNI_FALSE)
FILESYSTEM_STRING(maximum,jlong,0)
#undef FILESYSTEM_STRING

#define FILESYSTEM_BINDING(NAME,METHOD,SIGNATURE) \
    {"java/io/UnixFileSystem",METHOD,SIGNATURE,"Java_java_io_UnixFileSystem_" METHOD,(void**)&filesystem_original_##NAME,filesystem_##NAME,NULL}
#define FILESYSTEM_BINDINGS \
    FILESYSTEM_BINDING(canonical,"canonicalize0","(Ljava/lang/String;)Ljava/lang/String;"), \
    FILESYSTEM_BINDING(attributes,"getBooleanAttributes0","(Ljava/io/File;)I"), \
    FILESYSTEM_BINDING(access,"checkAccess0","(Ljava/io/File;I)Z"), \
    FILESYSTEM_BINDING(modified,"getLastModifiedTime0","(Ljava/io/File;)J"), \
    FILESYSTEM_BINDING(length,"getLength0","(Ljava/io/File;)J"), \
    FILESYSTEM_BINDING(permission,"setPermission0","(Ljava/io/File;IZZ)Z"), \
    FILESYSTEM_BINDING(create,"createFileExclusively0","(Ljava/lang/String;)Z"), \
    FILESYSTEM_BINDING(delete,"delete0","(Ljava/io/File;)Z"), \
    FILESYSTEM_BINDING(list,"list0","(Ljava/io/File;)[Ljava/lang/String;"), \
    FILESYSTEM_BINDING(directory,"createDirectory0","(Ljava/io/File;)Z"), \
    FILESYSTEM_BINDING(rename,"rename0","(Ljava/io/File;Ljava/io/File;)Z"), \
    FILESYSTEM_BINDING(time,"setLastModifiedTime0","(Ljava/io/File;J)Z"), \
    FILESYSTEM_BINDING(readonly,"setReadOnly0","(Ljava/io/File;)Z"), \
    FILESYSTEM_BINDING(space,"getSpace0","(Ljava/io/File;I)J"), \
    FILESYSTEM_BINDING(maximum,"getNameMax0","(Ljava/lang/String;)J")
