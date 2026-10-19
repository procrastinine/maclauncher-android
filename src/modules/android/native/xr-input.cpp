// Desktop-controlled virtual poses/actions use the pinned Klepton Mac viewer
// mappings. They do not represent headset tracking or a physical controller.
#include "xr-input.hpp"
#include <cmath>
#include <cstring>
extern "C" {
#include "kl_ovrp.h"
#include "kl_env.h"
}
void KlDesktopXrInput::event(SDL_Window *window,const SDL_Event &event,bool appHandled){
 const unsigned id=SDL_GetWindowID(window);
 if(event.type==SDL_EVENT_WINDOW_FOCUS_LOST&&event.window.windowID==id){release(window);return;}
 if(SDL_GetKeyboardFocus()!=window)return;
 if((event.type==SDL_EVENT_KEY_DOWN||event.type==SDL_EVENT_KEY_UP)&&event.key.windowID==id&&event.key.scancode<SDL_SCANCODE_COUNT){acceptedKeys[event.key.scancode]=event.type==SDL_EVENT_KEY_DOWN&&!appHandled;}
 if(event.type==SDL_EVENT_MOUSE_BUTTON_UP&&event.button.windowID==id){if(event.button.button==SDL_BUTTON_LEFT)mouse_l=0;if(event.button.button==SDL_BUTTON_RIGHT)mouse_r=0;}
 if(appHandled)return;
 if((event.type==SDL_EVENT_MOUSE_BUTTON_DOWN||event.type==SDL_EVENT_MOUSE_BUTTON_UP)&&event.button.windowID==id){
  bool down=event.type==SDL_EVENT_MOUSE_BUTTON_DOWN;
  if(down&&!mouselook)mouselook=SDL_SetWindowRelativeMouseMode(window,true)?1:0;
  if(event.button.button==SDL_BUTTON_LEFT)mouse_l=down;
  if(event.button.button==SDL_BUTTON_RIGHT)mouse_r=down;
 }else if(event.type==SDL_EVENT_MOUSE_MOTION&&event.motion.windowID==id&&mouselook){
  yaw-=event.motion.xrel*0.0025f;pitch-=event.motion.yrel*0.0025f;
  if(pitch>1.5f)pitch=1.5f;if(pitch<-1.5f)pitch=-1.5f;
 }else if(event.type==SDL_EVENT_KEY_DOWN&&event.key.windowID==id&&event.key.key==SDLK_ESCAPE)release(window);
}
void KlDesktopXrInput::release(SDL_Window *window){
 if(mouselook)SDL_SetWindowRelativeMouseMode(window,false);mouselook=mouse_l=mouse_r=0;
 memset(acceptedKeys,0,sizeof acceptedKeys);
 kl_ovrp_set_controller_input(0,0,0,0,0,0,0);kl_ovrp_set_controller_input(1,0,0,0,0,0,0);
}
void KlDesktopXrInput::tick(SDL_Window *window,bool enabled){
 uint64_t now=SDL_GetTicksNS();float dt=previous?float(now-previous)/1e9f:0;previous=now;if(dt>0.05f)dt=0.05f;
 bool soft_input_on=!enabled||SDL_GetKeyboardFocus()!=window;
 if(soft_input_on)release(window);
 const bool mono=false;
 #define KL_VIEW_MOVE_SPEED 2.0f
 #include "xr-input-generated.inc"
 #undef KL_VIEW_MOVE_SPEED
}
