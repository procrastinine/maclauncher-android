#include "native-gamepad.h"
#include <stdlib.h>
#include <string.h>

typedef struct Gamepad {
    SDL_JoystickID instance;
    SDL_Gamepad *handle;
    struct Gamepad *next;
} Gamepad;
struct KLJGamepads {
    jclass input;
    jmethodID added,removed,button,axis,reset;
    SDL_WindowID window;
    bool initialized,focused;
    Gamepad *devices;
};
static void fail(JNIEnv *env,const char *type,const char *message){
    if((*env)->ExceptionCheck(env))return;
    jclass klass=(*env)->FindClass(env,type);if(klass){(*env)->ThrowNew(env,klass,message);(*env)->DeleteLocalRef(env,klass);}
}
static Gamepad *find(KLJGamepads *owner,SDL_JoystickID instance){for(Gamepad *pad=owner->devices;pad;pad=pad->next)if(pad->instance==instance)return pad;return NULL;}
/* SDL names and paths are UTF-8, whereas JNI NewStringUTF expects modified UTF-8. */
static jstring string(JNIEnv *env,const char *text){
    if(!text)text="";
    char *utf16=SDL_iconv_string("UTF-16LE","UTF-8",text,strlen(text)+1);
    if(!utf16){fail(env,"java/lang/IllegalArgumentException","Controller metadata is not valid UTF-8");return NULL;}
    size_t length=0;const Uint16 *units=(const Uint16*)utf16;while(units[length])length++;
    jstring result=(*env)->NewString(env,(const jchar*)units,(jsize)length);SDL_free(utf16);return result;
}
static void snapshot(JNIEnv *env,KLJGamepads *owner,Gamepad *pad){
    for(int axis=0;axis<SDL_GAMEPAD_AXIS_COUNT&&!(*env)->ExceptionCheck(env);axis++)if(SDL_GamepadHasAxis(pad->handle,(SDL_GamepadAxis)axis))
        (*env)->CallStaticVoidMethod(env,owner->input,owner->axis,(jlong)pad->instance,(jint)axis,(jint)SDL_GetGamepadAxis(pad->handle,(SDL_GamepadAxis)axis));
    for(int button=0;button<SDL_GAMEPAD_BUTTON_COUNT&&!(*env)->ExceptionCheck(env);button++)if(SDL_GamepadHasButton(pad->handle,(SDL_GamepadButton)button)&&SDL_GetGamepadButton(pad->handle,(SDL_GamepadButton)button))
        (*env)->CallStaticVoidMethod(env,owner->input,owner->button,(jlong)pad->instance,(jint)button,JNI_TRUE);
}
static void describe(JNIEnv *env,KLJGamepads *owner,Gamepad *pad){
    if((*env)->PushLocalFrame(env,6)<0)return;
    jint buttons[SDL_GAMEPAD_BUTTON_COUNT],axes[SDL_GAMEPAD_AXIS_COUNT];int buttonCount=0,axisCount=0;
    for(int i=0;i<SDL_GAMEPAD_BUTTON_COUNT;i++)if(SDL_GamepadHasButton(pad->handle,(SDL_GamepadButton)i))buttons[buttonCount++]=i;
    for(int i=0;i<SDL_GAMEPAD_AXIS_COUNT;i++)if(SDL_GamepadHasAxis(pad->handle,(SDL_GamepadAxis)i))axes[axisCount++]=i;
    char guid[33];SDL_GUIDToString(SDL_GetGamepadGUIDForID(pad->instance),guid,sizeof(guid));
    const char *path=SDL_GetGamepadPath(pad->handle);char *descriptor=NULL;
    if(path&&*path)SDL_asprintf(&descriptor,"sdl:%s:%s",guid,path);else SDL_asprintf(&descriptor,"sdl:%s:%u",guid,pad->instance);
    if(!descriptor){fail(env,"java/lang/OutOfMemoryError","Controller descriptor allocation failed");goto done;}
    const char *label=SDL_GetGamepadName(pad->handle);
    jstring name=string(env,label?label:"SDL gamepad");if(!name){SDL_free(descriptor);goto done;}
    jstring identity=string(env,descriptor);SDL_free(descriptor);if(!identity)goto done;
    jintArray nativeButtons=(*env)->NewIntArray(env,buttonCount);if(!nativeButtons)goto done;
    jintArray nativeAxes=(*env)->NewIntArray(env,axisCount);if(!nativeAxes)goto done;
    (*env)->SetIntArrayRegion(env,nativeButtons,0,buttonCount,buttons);if((*env)->ExceptionCheck(env))goto done;
    (*env)->SetIntArrayRegion(env,nativeAxes,0,axisCount,axes);if((*env)->ExceptionCheck(env))goto done;
    (*env)->CallStaticVoidMethod(env,owner->input,owner->added,(jlong)pad->instance,name,identity,(jint)SDL_GetGamepadVendor(pad->handle),(jint)SDL_GetGamepadProduct(pad->handle),nativeButtons,nativeAxes);
done:
    (void)(*env)->ExceptionCheck(env);
    (*env)->PopLocalFrame(env,NULL);
    if(owner->focused&&!(*env)->ExceptionCheck(env))snapshot(env,owner,pad);
}
static void open_gamepad(JNIEnv *env,KLJGamepads *owner,SDL_JoystickID instance){
    if(find(owner,instance))return;
    SDL_Gamepad *handle=SDL_OpenGamepad(instance);if(!handle)return; // A hot-unplug may race discovery.
    Gamepad *pad=calloc(1,sizeof(*pad));if(!pad){SDL_CloseGamepad(handle);fail(env,"java/lang/OutOfMemoryError","Controller owner allocation failed");return;}
    pad->instance=instance;pad->handle=handle;pad->next=owner->devices;owner->devices=pad;describe(env,owner,pad);
}
static void remove_gamepad(JNIEnv *env,KLJGamepads *owner,SDL_JoystickID instance){
    Gamepad **link=&owner->devices;while(*link&&(*link)->instance!=instance)link=&(*link)->next;
    if(!*link)return;Gamepad *pad=*link;*link=pad->next;
    (*env)->CallStaticVoidMethod(env,owner->input,owner->removed,(jlong)pad->instance);SDL_CloseGamepad(pad->handle);free(pad);
}
KLJGamepads *klj_gamepads_create(JNIEnv *env,SDL_Window *window){
    if(!window){fail(env,"java/lang/IllegalArgumentException","Controller input requires a native window");return NULL;}
    KLJGamepads *owner=calloc(1,sizeof(*owner));if(!owner){fail(env,"java/lang/OutOfMemoryError","Controller owner allocation failed");return NULL;}
    owner->window=SDL_GetWindowID(window); // Publish held controls only after the Android root gains focus.
    jclass local=(*env)->FindClass(env,"maclauncher/android/framework/HostInput");if(!local)goto failed;
    owner->input=(*env)->NewGlobalRef(env,local);(*env)->DeleteLocalRef(env,local);if(!owner->input)goto failed;
    owner->added=(*env)->GetStaticMethodID(env,owner->input,"gamepadAdded","(JLjava/lang/String;Ljava/lang/String;II[I[I)V");if(!owner->added)goto failed;
    owner->removed=(*env)->GetStaticMethodID(env,owner->input,"gamepadRemoved","(J)V");if(!owner->removed)goto failed;
    owner->reset=(*env)->GetStaticMethodID(env,owner->input,"gamepadReset","(J)V");if(!owner->reset)goto failed;
    owner->button=(*env)->GetStaticMethodID(env,owner->input,"gamepadButton","(JIZ)V");if(!owner->button)goto failed;
    owner->axis=(*env)->GetStaticMethodID(env,owner->input,"gamepadAxis","(JII)V");if(!owner->axis)goto failed;
    if(!SDL_InitSubSystem(SDL_INIT_GAMEPAD)){SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,"Controller input is unavailable: %s",SDL_GetError());return owner;}
    owner->initialized=true;int count=0;SDL_JoystickID *devices=SDL_GetGamepads(&count);
    if(!devices){fail(env,"java/lang/IllegalStateException",SDL_GetError());goto failed;}
    for(int i=0;i<count&&!(*env)->ExceptionCheck(env);i++)open_gamepad(env,owner,devices[i]);SDL_free(devices);
    if((*env)->ExceptionCheck(env))goto failed;return owner;
failed:
    klj_gamepads_close(env,owner);return NULL;
}
int klj_gamepads_event(JNIEnv *env,KLJGamepads *owner,const SDL_Event *event){
    if(!owner||!owner->initialized||!event)return 0;
    int handled=1;Gamepad *pad;
    switch(event->type){
        case SDL_EVENT_GAMEPAD_ADDED:open_gamepad(env,owner,event->gdevice.which);break;
        case SDL_EVENT_GAMEPAD_REMOVED:remove_gamepad(env,owner,event->gdevice.which);break;
        case SDL_EVENT_GAMEPAD_REMAPPED:pad=find(owner,event->gdevice.which);if(pad)describe(env,owner,pad);break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:case SDL_EVENT_GAMEPAD_BUTTON_UP:
            if(owner->focused&&find(owner,event->gbutton.which))(*env)->CallStaticVoidMethod(env,owner->input,owner->button,(jlong)event->gbutton.which,(jint)event->gbutton.button,(jboolean)event->gbutton.down);break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if(owner->focused&&find(owner,event->gaxis.which))(*env)->CallStaticVoidMethod(env,owner->input,owner->axis,(jlong)event->gaxis.which,(jint)event->gaxis.axis,(jint)event->gaxis.value);break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:case SDL_EVENT_WINDOW_FOCUS_GAINED:
            handled=0;if(event->window.windowID!=owner->window)break;
            owner->focused=event->type==SDL_EVENT_WINDOW_FOCUS_GAINED;
            for(pad=owner->devices;pad&&!(*env)->ExceptionCheck(env);pad=pad->next){
                if(owner->focused)snapshot(env,owner,pad);else (*env)->CallStaticVoidMethod(env,owner->input,owner->reset,(jlong)pad->instance);
            }break;
        default:handled=0;break;
    }
    return (*env)->ExceptionCheck(env)?-1:handled;
}
void klj_gamepads_close(JNIEnv *env,KLJGamepads *owner){
    if(!owner)return;
    jthrowable failure=(*env)->ExceptionOccurred(env);if(failure)(*env)->ExceptionClear(env);
    while(owner->devices){
        remove_gamepad(env,owner,owner->devices->instance);
        jthrowable next=(*env)->ExceptionOccurred(env);if(next){(*env)->ExceptionClear(env);if(!failure)failure=next;else (*env)->DeleteLocalRef(env,next);}
    }
    if(owner->input)(*env)->DeleteGlobalRef(env,owner->input);
    if(owner->initialized)SDL_QuitSubSystem(SDL_INIT_GAMEPAD);free(owner);
    if(failure){(*env)->Throw(env,failure);(*env)->DeleteLocalRef(env,failure);}
}
