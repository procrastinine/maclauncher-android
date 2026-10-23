// Desktop-controlled virtual poses/actions use the pinned Klepton Mac viewer
// mappings. They do not represent headset tracking or a physical controller.
#include "xr-input.hpp"
#include <cmath>
#include <cstring>
extern "C" {
#include "kl_ovrp.h"
#include "kl_env.h"
int klj_openxr_input_active(void);
int klj_ovrp_input_active(void);
}
bool KlDesktopXrInput::active(){return (klj_openxr_input_active()||klj_ovrp_input_active())&&kl_ovrp_last_complete_stage()>=0;}
static bool mapped(SDL_Scancode key){
 switch(key){
  case SDL_SCANCODE_W:case SDL_SCANCODE_A:case SDL_SCANCODE_S:case SDL_SCANCODE_D:
  case SDL_SCANCODE_R:case SDL_SCANCODE_F:case SDL_SCANCODE_SPACE:case SDL_SCANCODE_LSHIFT:
  case SDL_SCANCODE_G:case SDL_SCANCODE_H:case SDL_SCANCODE_Z:case SDL_SCANCODE_X:
  case SDL_SCANCODE_C:case SDL_SCANCODE_V:case SDL_SCANCODE_Q:case SDL_SCANCODE_E:
  case SDL_SCANCODE_UP:case SDL_SCANCODE_DOWN:case SDL_SCANCODE_LEFT:case SDL_SCANCODE_RIGHT:return true;
  default:return false;
 }
}
bool KlDesktopXrInput::event(SDL_Window *window,const SDL_Event &event,bool enabled){
 const unsigned id=SDL_GetWindowID(window);
 if(event.type==SDL_EVENT_WINDOW_FOCUS_LOST&&event.window.windowID==id){release(window);return false;}
 enabled=enabled&&SDL_GetKeyboardFocus()==window;
 if(!enabled)release(window);
 if((event.type==SDL_EVENT_KEY_DOWN||event.type==SDL_EVENT_KEY_UP)&&event.key.windowID==id&&event.key.scancode>=0&&event.key.scancode<SDL_SCANCODE_COUNT){
  auto key=event.key.scancode;
  if(event.type==SDL_EVENT_KEY_UP){bool captured=capturedKeys[key];capturedKeys[key]=acceptedKeys[key]=false;return captured;}
  if(event.key.mod&(SDL_KMOD_GUI|SDL_KMOD_CTRL|SDL_KMOD_ALT)){capturedKeys[key]=acceptedKeys[key]=false;return false;}
  if(event.key.repeat&&capturedKeys[key])return true;
  if(!enabled){capturedKeys[key]=acceptedKeys[key]=false;return false;}
  if(event.key.key==SDLK_ESCAPE&&mouselook){release(window);capturedKeys[key]=true;return true;}
  if(mapped(key)){capturedKeys[key]=acceptedKeys[key]=true;return true;}
 }
 if((event.type==SDL_EVENT_MOUSE_BUTTON_DOWN||event.type==SDL_EVENT_MOUSE_BUTTON_UP)&&event.button.windowID==id){
  unsigned bit=event.button.button==SDL_BUTTON_LEFT?1:event.button.button==SDL_BUTTON_RIGHT?2:0;if(!bit)return false;
  bool down=event.type==SDL_EVENT_MOUSE_BUTTON_DOWN;
  if(!down){bool captured=(capturedMouse&bit)!=0;capturedMouse&=~bit;if(bit==1)mouse_l=0;else mouse_r=0;return captured;}
  if(!enabled||(SDL_GetModState()&(SDL_KMOD_GUI|SDL_KMOD_CTRL|SDL_KMOD_ALT))){capturedMouse&=~bit;return false;}
  if(!mouselook)mouselook=SDL_SetWindowRelativeMouseMode(window,true)?1:0;
  if(!mouselook)return false;
  capturedMouse|=bit;
  if(event.button.button==SDL_BUTTON_LEFT)mouse_l=down;
  if(event.button.button==SDL_BUTTON_RIGHT)mouse_r=down;
  return true;
 }else if(event.type==SDL_EVENT_MOUSE_MOTION&&event.motion.windowID==id){
  if(enabled&&mouselook){yaw-=event.motion.xrel*0.0025f;pitch-=event.motion.yrel*0.0025f;if(pitch>1.5f)pitch=1.5f;if(pitch<-1.5f)pitch=-1.5f;return true;}
  return ((capturedMouse&1)&&(event.motion.state&SDL_BUTTON_LMASK))||((capturedMouse&2)&&(event.motion.state&SDL_BUTTON_RMASK));
 }
 return false;
}
void KlDesktopXrInput::release(SDL_Window *window){
 if(mouselook)SDL_SetWindowRelativeMouseMode(window,false);mouselook=mouse_l=mouse_r=0;
 memset(acceptedKeys,0,sizeof acceptedKeys);
 kl_ovrp_set_controller_input(0,0,0,0,0,0,0);kl_ovrp_set_controller_input(1,0,0,0,0,0,0);
}
void KlDesktopXrInput::tick(SDL_Window *window,bool enabled){
 uint64_t now=SDL_GetTicksNS();float dt=previous?float(now-previous)/1e9f:0;previous=now;if(dt>0.05f)dt=0.05f;
 bool soft_input_on=!enabled||SDL_GetKeyboardFocus()!=window;
 // Focus controls actions, not the stationary tracking sample. The original
 // viewer still publishes head/hand poses while its keyboard is unavailable.
 if(soft_input_on)release(window);
 const bool mono=false;
 #define KL_VIEW_MOVE_SPEED 2.0f
 #include "xr-input-generated.inc"
 #undef KL_VIEW_MOVE_SPEED
}
