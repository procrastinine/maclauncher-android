#pragma once
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <SDL3/SDL.h>
#include <string>
#include "window-placement.h"
#include "text-input-area.h"
#include "application-platform.h"

/* Stateless Mac operations shared by VM-specific session adapters. The caller
 * owns the SDL window/view and the session's callback and producer lifetime. */
static inline bool kl_window_physical_text_area(SDL_Window*window,int sourceWidth,int sourceHeight,float left,float top,float right,float bottom){
 int width=0,height=0;if(!SDL_GetWindowSize(window,&width,&height))return false;
 SDL_Rect area;if(!klj_text_input_area(left,top,right,bottom,sourceWidth,sourceHeight,width,height,&area))return false;
 return SDL_SetTextInputArea(window,&area,0);
}
static inline KLWindowPlacement kl_window_physical_placement(SDL_Window* window,int width,int height){
 SDL_Rect bounds{};SDL_DisplayID display=SDL_GetDisplayForWindow(window);
 if(!(SDL_GetWindowFlags(window)&(SDL_WINDOW_FULLSCREEN|SDL_WINDOW_MAXIMIZED))&&display&&SDL_GetDisplayUsableBounds(display,&bounds)){
  int top=0,left=0,bottom=0,right=0;
  if(!SDL_GetWindowBordersSize(window,&top,&left,&bottom,&right)){
   NSWindow*cocoa=(__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(window),SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,nullptr);
   if(cocoa){NSRect frame=cocoa.frame,content=[cocoa contentRectForFrameRect:frame];
    top=(int)ceil(NSMaxY(frame)-NSMaxY(content));left=(int)ceil(NSMinX(content)-NSMinX(frame));
    bottom=(int)ceil(NSMinY(content)-NSMinY(frame));right=(int)ceil(NSMaxX(frame)-NSMaxX(content));}
  }
  return kl_window_placement(width,height,bounds.x,bounds.y,bounds.w,bounds.h,top,left,bottom,right);
 }
 int x=0,y=0;SDL_GetWindowPosition(window,&x,&y);return {x,y,width,height};
}
static inline bool kl_window_physical_place(SDL_Window* window,int width,int height){
 auto placement=kl_window_physical_placement(window,width,height);
 return SDL_SetWindowSize(window,placement.width,placement.height)&&SDL_SetWindowPosition(window,placement.x,placement.y);
}
static inline SDL_Window* kl_window_physical_create(const char*title,int width,int height,SDL_WindowFlags additional=0){
 SDL_Window*window=SDL_CreateWindow(title,width,height,SDL_WINDOW_METAL|SDL_WINDOW_RESIZABLE|SDL_WINDOW_HIGH_PIXEL_DENSITY|additional);
 if(window){NSWindow*cocoa=(__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(window),SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,nullptr);cocoa.restorable=NO;}
 if(window&&!kl_window_physical_place(window,width,height)){std::string error=SDL_GetError();SDL_DestroyWindow(window);SDL_SetError("%s",error.c_str());return nullptr;}
 return window;
}
static inline SDL_MetalView kl_window_physical_layer(SDL_Window*window){
 if(!SDL_SetHintWithPriority(SDL_HINT_VIDEO_METAL_AUTO_RESIZE_DRAWABLE,"0",SDL_HINT_OVERRIDE)){SDL_SetError("Native drawable ownership unavailable");return nullptr;}
 return SDL_Metal_CreateView(window);
}
static inline bool kl_window_physical_drawable(SDL_Window*window,SDL_MetalView view,int*outWidth,int*outHeight){@autoreleasepool{
 CAMetalLayer*layer=(__bridge CAMetalLayer*)SDL_Metal_GetLayer(view);
 if(!layer)return SDL_SetError("Native Metal layer is unavailable");
 layer.device=MTLCreateSystemDefaultDevice();if(!layer.device)return SDL_SetError("Metal device is unavailable");
 int width=0,height=0;
 if(!SDL_GetWindowSizeInPixels(window,&width,&height)||width<1||height<1||width>8192||height>8192)return SDL_SetError("Native backing extent is unavailable");
 layer.pixelFormat=MTLPixelFormatBGRA8Unorm;layer.framebufferOnly=NO;layer.maximumDrawableCount=3;layer.drawableSize=CGSizeMake(width,height);layer.allowsNextDrawableTimeout=YES;
 id<CAMetalDrawable> image=[layer nextDrawable];if(!image)return SDL_SetError("Initial Android drawable unavailable within platform timeout");
 *outWidth=(int)image.texture.width;*outHeight=(int)image.texture.height;
 if(*outWidth!=width||*outHeight!=height)return SDL_SetError("Initial Android drawable extent differs from committed viewport");
 /* A borrowed validation drawable: no command buffer or presentation. */
 image=nil;return true;
}}
