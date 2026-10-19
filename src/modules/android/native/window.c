#include "window.h"
#include <ApplicationServices/ApplicationServices.h>
#include <pthread.h>
#include <stdio.h>

int klj_window_prepare(JNIEnv *env){
    OSStatus status=paramErr;
    if(pthread_main_np()){
        // The command-line Java launcher does not perform AWT's foreground
        // process registration. SDL owns AppKit after this one-time transform.
        ProcessSerialNumber process;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        status=GetCurrentProcess(&process);
        if(status==noErr)status=TransformProcessType(&process,kProcessTransformToForegroundApplication);
#pragma clang diagnostic pop
    }
    if(status!=noErr){
        char message[128];snprintf(message,sizeof message,"Android window registration failed (%d)",(int)status);
        jclass type=(*env)->FindClass(env,"java/lang/IllegalStateException");
        if(type)(*env)->ThrowNew(env,type,message);
        return 0;
    }
    return 1;
}
