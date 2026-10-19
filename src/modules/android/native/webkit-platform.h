#pragma once
#import <Foundation/Foundation.h>
#include <cstdint>
#include <memory>

// AppKit/WebKit owns documents, input, storage and pixels. A VM binding owns
// callback references and consumes borrowed snapshot bytes before returning.
struct KljWebClient {
    virtual ~KljWebClient()=default;
    virtual void* contentView()=0;
    virtual void changed()=0;
    virtual void dimensions(int width,int height)=0;
    virtual void navigation(NSString* state,NSString* url,NSString* title,int progress)=0;
    virtual void history(NSArray<NSString*>* urls,NSArray<NSString*>* titles,int index)=0;
    virtual void dialog(int64_t id,NSString* kind,NSString* url,NSString* message,NSString* value)=0;
    virtual NSData* resource(NSString* url)=0;
    virtual void editor(NSString* json)=0;
    virtual void port(int64_t id,NSString* text)=0;
    virtual bool navigationRequest(NSString* url,bool main,bool gesture,bool redirect,NSString* method,NSString* headers)=0;
    virtual NSString* interface(NSString* name,NSString* method,NSString* arguments)=0;
    virtual void script(int64_t request,NSString* json,NSString* error)=0;
    virtual void frame(const void* pixels,int width,int height,NSString* error)=0;
};
struct KljWebResult {
    virtual ~KljWebResult()=default;
    virtual void complete(NSString* json,NSString* error)=0;
};
struct KljWebContext;
std::shared_ptr<KljWebContext> klj_webkit_context();
void klj_webkit_retire(const std::shared_ptr<KljWebContext>& context);
bool klj_webkit_owns(const std::shared_ptr<KljWebContext>& context,int64_t handle);
NSString* klj_webkit_initialize(std::shared_ptr<KljWebContext> context,NSString* directory,NSString* files,bool allowed);
int64_t klj_webkit_create(std::shared_ptr<KljWebContext> context,std::shared_ptr<KljWebClient> callback);
void klj_webkit_destroy(int64_t handle);
void klj_webkit_visibility(int64_t handle,bool visible);
void klj_webkit_focus(int64_t handle,bool focused);
void klj_webkit_size(int64_t handle,int width,int height,float density);
void klj_webkit_load(int64_t handle,NSString* url,NSArray<NSString*>* headers,NSArray<NSString*>* values,NSData* body);
void klj_webkit_html(int64_t handle,NSString* html,NSString* base);
void klj_webkit_script(int64_t handle,NSString* script,int64_t request);
void klj_webkit_navigate(int64_t handle,int delta);
void klj_webkit_reload(int64_t handle);
void klj_webkit_stop(int64_t handle);
void klj_webkit_snapshot(int64_t handle);
void klj_webkit_touch(int64_t handle,int action,float x,float y);
void klj_webkit_key(int64_t handle,int action,int key,NSString* text,int modifiers);
void klj_webkit_edit(int64_t handle,int operation,NSString* text,int start,int length);
void klj_webkit_editor(int64_t handle,NSString* before,NSString* after,NSString* metadata);
void klj_webkit_setting(int64_t handle,NSString* name,NSString* json);
void klj_webkit_interfaces(int64_t handle,NSString* json);
void klj_webkit_dialogResult(int64_t handle,int64_t request,bool accepted,NSString* value);
NSData* klj_webkit_state(int64_t handle);
bool klj_webkit_restore(int64_t handle,NSData* state);
void klj_webkit_command(int64_t handle,NSString* operation,NSString* json,int64_t request);
NSString* klj_webkit_cookies(std::shared_ptr<KljWebContext> context,NSString* url);
void klj_webkit_cookieCommand(std::shared_ptr<KljWebContext> context,NSString* operation,NSString* json,std::shared_ptr<KljWebResult> completion);
void klj_webkit_storageCommand(std::shared_ptr<KljWebContext> context,NSString* operation,NSString* json,std::shared_ptr<KljWebResult> completion);
extern "C" void klj_webkit_window_detach(void* window);
