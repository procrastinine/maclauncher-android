#include "display-snapshot.h"
#import <AppKit/AppKit.h>
#import <CoreVideo/CoreVideo.h>
#include <cmath>
#include <climits>
#include <cstdio>
#include <pthread.h>
#include <jni.h>
static int fail(char* error,size_t capacity,const char* text){if(error&&capacity)snprintf(error,capacity,"%s",text);return 0;}
static int screen_snapshot(CGDirectDisplayID screen,CGSize points,KLDisplaySnapshot* result,char* error,size_t capacity){
    if(!result)return fail(error,capacity,"Display snapshot output is missing");
    if(!screen)return fail(error,capacity,"Current native screen is unavailable");
    CGDisplayModeRef mode=CGDisplayCopyDisplayMode(screen);
    if(!mode)return fail(error,capacity,"Current native display mode is unavailable");
    uint32_t id=CGDisplayModeGetIODisplayModeID(mode);
    size_t width=CGDisplayModeGetPixelWidth(mode),height=CGDisplayModeGetPixelHeight(mode);
    double refresh=CGDisplayModeGetRefreshRate(mode);CGDisplayModeRelease(mode);
    int source=1;
    if(!std::isfinite(refresh)||refresh<=0){
        CVDisplayLinkRef link=nullptr;CVReturn code=CVDisplayLinkCreateWithCGDisplay(screen,&link);
        if(code!=kCVReturnSuccess||!link){if(link)CVDisplayLinkRelease(link);return fail(error,capacity,"Current screen refresh period is unavailable");}
        CVTime period=CVDisplayLinkGetNominalOutputVideoRefreshPeriod(link);CVDisplayLinkRelease(link);
        if(period.flags&kCVTimeIsIndefinite||period.timeValue<=0||period.timeScale<=0)return fail(error,capacity,"Current screen refresh period is indefinite");
        refresh=static_cast<double>(period.timeScale)/static_cast<double>(period.timeValue);source=2;
    }
    if(!width||width>INT_MAX||!height||height>INT_MAX||!std::isfinite(refresh)||refresh<=0||!std::isfinite(static_cast<float>(refresh)))
        return fail(error,capacity,"Native display mode cannot be represented by the Android display API");
    if(!std::isfinite(points.width)||!std::isfinite(points.height)||points.width<=0||points.height<=0)return fail(error,capacity,"Native content size is unavailable");
    *result={screen,id,static_cast<int32_t>(width),static_cast<int32_t>(height),static_cast<float>(refresh),source,static_cast<float>(points.width),static_cast<float>(points.height)};return 1;
}
extern "C" int kl_display_snapshot_read(void* cocoaWindow,KLDisplaySnapshot* result,char* error,size_t capacity){
    if(!pthread_main_np())return fail(error,capacity,"Display observation requires the native UI thread");
    @autoreleasepool {
        NSWindow* window=(__bridge NSWindow*)cocoaWindow;
        if(!window||![window isKindOfClass:NSWindow.class])return fail(error,capacity,"Display observation requires the owned native window");
        CGDirectDisplayID screen=[window.screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
        return screen_snapshot(screen,window.contentView.bounds.size,result,error,capacity);
    }
}
extern "C" int kl_display_snapshot_screen(uint32_t screen,KLDisplaySnapshot* result,char* error,size_t capacity){
    return screen_snapshot(screen,CGDisplayBounds(screen).size,result,error,capacity);
}
extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostDisplay_initializeHeadless(JNIEnv* env,jclass owner){
    KLDisplaySnapshot snapshot;char error[256];CGDirectDisplayID screen=CGMainDisplayID();
    if(!screen_snapshot(screen,CGDisplayBounds(screen).size,&snapshot,error,sizeof(error))){
        jclass type=env->FindClass("java/lang/IllegalStateException");if(type){env->ThrowNew(type,error);env->DeleteLocalRef(type);}return;
    }
    jmethodID publish=env->GetStaticMethodID(owner,"hostPublishScreen","(JIIFFZ)V");if(!publish)return;
    env->CallStaticVoidMethod(owner,publish,(jlong)snapshot.screenId,(jint)snapshot.physicalWidth,(jint)snapshot.physicalHeight,(jfloat)snapshot.refreshRate,(jfloat)(snapshot.physicalWidth/snapshot.windowWidth),(jboolean)CGDisplayIsBuiltin(screen));
    env->ExceptionCheck();
}
