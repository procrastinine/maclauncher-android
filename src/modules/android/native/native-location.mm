#import <AppKit/AppKit.h>
#include <SDL3/SDL.h>
#include <jni.h>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <memory>
#include <mutex>

namespace {
struct Environment {
    JavaVM *vm;
    JNIEnv *env = nullptr;
    bool attached = false;
    explicit Environment(JavaVM *value) : vm(value) {
        const jint status = vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
        if (status == JNI_EDETACHED) {
            attached = vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void **>(&env), nullptr) == JNI_OK;
            if (!attached) env = nullptr;
        } else if (status != JNI_OK) env = nullptr;
    }
    ~Environment() {
        if (attached) {
            // A newly attached AppKit callback has no Java caller to receive an
            // exception. Preserve its diagnostic before detaching the thread.
            if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
            vm->DetachCurrentThread();
        }
    }
};
void fail(JNIEnv *env, const char *message) {
    if (env->ExceptionCheck()) return;
    jclass type = env->FindClass("java/lang/IllegalStateException");
    if (type) { env->ThrowNew(type, message); env->DeleteLocalRef(type); }
}
bool number(NSTextField *field, double minimum, double maximum, double &value) {
    NSString *text = field.stringValue;
    if (!text.length || text.length > 64) return false;
    NSScanner *scanner = [NSScanner scannerWithString:text];
    scanner.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    return [scanner scanDouble:&value] && scanner.isAtEnd && std::isfinite(value)
        && value >= minimum && value <= maximum;
}
}

@interface MLManualLocationFields : NSObject <NSTextFieldDelegate>
@property(nonatomic,strong) NSTextField *latitude;
@property(nonatomic,strong) NSTextField *longitude;
@property(nonatomic,strong) NSTextField *accuracy;
@property(nonatomic,strong) NSButton *share;
- (BOOL)read:(double *)coordinates;
- (void)refresh;
@end
@implementation MLManualLocationFields
- (BOOL)read:(double *)coordinates {
    return number(self.latitude, -90, 90, coordinates[0])
        && number(self.longitude, -180, 180, coordinates[1])
        && number(self.accuracy, 0, 1000000, coordinates[2]);
}
- (void)refresh { double coordinates[3]; self.share.enabled = [self read:coordinates]; }
- (void)controlTextDidChange:(NSNotification *)notification { [self refresh]; }
@end

namespace {
struct Request {
    JavaVM *vm = nullptr;
    jclass callback = nullptr;
    jmethodID complete = nullptr;
    jlong identity = 0;
    std::atomic<bool> cancelled{false}, finished{false};
    __strong NSAlert *alert = nil;
    __strong MLManualLocationFields *fields = nil;
    __strong id closeObserver = nil;
    __weak NSWindow *window = nil;
    ~Request() {
        if (callback) { Environment scope(vm); if (scope.env) scope.env->DeleteGlobalRef(callback); }
    }
};
std::mutex requestMutex;
std::shared_ptr<Request> active;

void finish(const std::shared_ptr<Request> &request, const double *coordinates) {
    if (request->finished.exchange(true)) return;
    if (request->closeObserver) {
        [[NSNotificationCenter defaultCenter] removeObserver:request->closeObserver];
        request->closeObserver = nil;
    }
    [request->alert.window orderOut:nil];
    request->alert = nil;
    request->fields = nil;
    { std::lock_guard lock(requestMutex); if (active == request) active.reset(); }
    Environment scope(request->vm);
    JNIEnv *env = scope.env;
    if (!env) return;
    // Do not replace a pending guest failure with a late UI callback.
    if (!env->ExceptionCheck()) {
        jdoubleArray result = nullptr;
        if (coordinates && !request->cancelled.load()) {
            result = env->NewDoubleArray(3);
            if (result) env->SetDoubleArrayRegion(result, 0, 3, coordinates);
        }
        if (!env->ExceptionCheck()) env->CallStaticVoidMethod(request->callback, request->complete, request->identity, result);
        if (result) env->DeleteLocalRef(result);
    }
    env->DeleteGlobalRef(request->callback);
    request->callback = nullptr;
}

void cancelOnMain(const std::shared_ptr<Request> &request) {
    if (request->finished.load()) return;
    if (request->alert.window.sheetParent)
        [request->alert.window.sheetParent endSheet:request->alert.window returnCode:NSAlertSecondButtonReturn];
    finish(request, nullptr);
}

NSWindow *ownedWindow() {
    // The host has one SDL window. Never attach to another AppKit panel or a
    // process selected merely because it happens to be frontmost.
    int count = 0;
    SDL_Window **windows = SDL_GetWindows(&count);
    NSWindow *window = nil;
    if (windows && count == 1) window = (__bridge NSWindow *)SDL_GetPointerProperty(
        SDL_GetWindowProperties(windows[0]), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    SDL_free(windows);
    return window;
}

NSTextField *field(NSView *view, NSString *label, NSString *value, CGFloat y) {
    NSTextField *caption = [NSTextField labelWithString:label];
    caption.frame = NSMakeRect(0, y + 3, 176, 22);
    [view addSubview:caption];
    NSTextField *input = [[NSTextField alloc] initWithFrame:NSMakeRect(182, y, 174, 26)];
    input.stringValue = value;
    input.accessibilityLabel = label;
    [view addSubview:input];
    return input;
}

void show(const std::shared_ptr<Request> &request) {
    if (request->cancelled.load()) { finish(request, nullptr); return; }
    NSWindow *window = ownedWindow();
    if (!window) { finish(request, nullptr); return; }
    request->window = window;
    NSAlert *alert = [NSAlert new];
    request->alert = alert;
    alert.messageText = @"Share a location";
    alert.informativeText = @"An Android app requested location. Enter coordinates to share for this session.";
    [alert addButtonWithTitle:@"Share"];
    [alert addButtonWithTitle:@"Cancel"];
    NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 356, 112)];
    MLManualLocationFields *fields = [MLManualLocationFields new];
    request->fields = fields;
    fields.latitude = field(view, @"Latitude (−90 to 90)", @"", 80);
    fields.longitude = field(view, @"Longitude (−180 to 180)", @"", 44);
    fields.accuracy = field(view, @"Accuracy (meters)", @"100", 8);
    fields.latitude.delegate = fields;
    fields.longitude.delegate = fields;
    fields.accuracy.delegate = fields;
    fields.share = alert.buttons[0];
    [fields refresh];
    alert.accessoryView = view;
    alert.window.initialFirstResponder = fields.latitude;
    std::weak_ptr<Request> weak = request;
    request->closeObserver = [[NSNotificationCenter defaultCenter]
        addObserverForName:NSWindowWillCloseNotification object:window queue:nil usingBlock:^(NSNotification *) {
            if (auto pending = weak.lock()) { pending->cancelled = true; cancelOnMain(pending); }
        }];
    // The completion outlives the caller's shared_ptr storage.
    auto completionRequest = request;
    [alert beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
        double coordinates[3];
        const bool shared = response == NSAlertFirstButtonReturn && [completionRequest->fields read:coordinates];
        finish(completionRequest, shared ? coordinates : nullptr);
    }];
}
}

extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NativeLocation_request(JNIEnv *env, jclass, jlong identity) {
    if (env->ExceptionCheck()) return;
    if (identity <= 0) { fail(env, "Location request identity is invalid"); return; }
    auto request = std::make_shared<Request>();
    request->identity = identity;
    if (env->GetJavaVM(&request->vm) != JNI_OK) { fail(env, "Location callback VM is unavailable"); return; }
    jclass callback = env->FindClass("maclauncher/android/location/ManualLocation");
    if (!callback) return;
    request->complete = env->GetStaticMethodID(callback, "complete", "(J[D)V");
    if (request->complete) request->callback = static_cast<jclass>(env->NewGlobalRef(callback));
    env->DeleteLocalRef(callback);
    if (!request->callback || env->ExceptionCheck()) return;
    {
        std::lock_guard lock(requestMutex);
        if (active) {
            if (active->identity != identity) fail(env, "A location request is already pending");
            return;
        }
        active = request;
    }
    dispatch_async(dispatch_get_main_queue(), ^{ @autoreleasepool { show(request); } });
}

extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NativeLocation_cancel(JNIEnv *, jclass, jlong identity) {
    std::shared_ptr<Request> request;
    { std::lock_guard lock(requestMutex); if (active && active->identity == identity && !active->cancelled.exchange(true)) request = active; }
    if (!request) return;
    if ([NSThread isMainThread]) cancelOnMain(request);
    else dispatch_async(dispatch_get_main_queue(), ^{ @autoreleasepool { cancelOnMain(request); } });
}
