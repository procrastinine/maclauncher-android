#include <jni.h>

// Reuse HotSpot's ZIP64/local-header calculation, including prepended archive
// data. JNI accesses the JDK's actual stream owner without copying ZIP parsing.
JNIEXPORT jlong JNICALL Java_maclauncher_android_framework_HostStrictJarFile_entryDataOffset(JNIEnv *env,jclass ignored,jobject stream){
    (void)ignored;
    jclass owner=(*env)->FindClass(env,"java/util/zip/ZipFile$ZipFileInputStream");if(!owner)return -1;
    jobject raw=stream;
    if(!(*env)->IsInstanceOf(env,raw,owner)){
        jclass filter=(*env)->FindClass(env,"java/io/FilterInputStream");if(!filter){(*env)->DeleteLocalRef(env,owner);return -1;}
        jfieldID input=(*env)->GetFieldID(env,filter,"in","Ljava/io/InputStream;");
        if(input&&(*env)->IsInstanceOf(env,stream,filter))raw=(*env)->GetObjectField(env,stream,input);
        (*env)->DeleteLocalRef(env,filter);
        if((*env)->ExceptionCheck(env)){(*env)->DeleteLocalRef(env,owner);return -1;}
    }
    jlong offset=-1;
    if(raw&&(*env)->IsInstanceOf(env,raw,owner)){
        jmethodID initialize=(*env)->GetMethodID(env,owner,"initDataOffset","()J");
        if(initialize){offset=(*env)->CallLongMethod(env,raw,initialize);if((*env)->ExceptionCheck(env))offset=-1;}
    }else{
        jclass error=(*env)->FindClass(env,"java/io/IOException");
        if(error){(*env)->ThrowNew(env,error,"ZIP stream has no native data offset");(*env)->DeleteLocalRef(env,error);}
    }
    if(raw&&raw!=stream)(*env)->DeleteLocalRef(env,raw);
    (*env)->DeleteLocalRef(env,owner);return offset;
}
