#pragma once
#include <SDL3/SDL.h>
#include <string.h>
#include "input-key.h"
#include "resolution.h"

/* VM-neutral physical ingress. Android owns editor, touch, scroll and clipboard
 * behavior; adapters only deliver this same decoded event through JNI or IPC. */
enum KLWindowInputKind {KL_INPUT_NONE,KL_INPUT_FOCUS,KL_INPUT_TOUCH,KL_INPUT_KEY,
 KL_INPUT_WHEEL,KL_INPUT_CLOSE,KL_INPUT_TEXT,KL_INPUT_SHORTCUT,KL_INPUT_CONSUMED,
 KL_INPUT_SLEEP=10};
typedef struct {uint32_t kind;int32_t action,code,meta;float x,y,dx,dy;} KLWindowInput;
typedef struct {KLWindowInput value;const char *text;int start,length,unicode;} KLWindowEvent;

static inline void kl_window_input_text_mode(SDL_Window *window,int enabled){
 int active=SDL_TextInputActive(window);
 if(enabled&&!active)SDL_StartTextInput(window);
 else if(!enabled){if(active)SDL_StopTextInput(window);SDL_SetTextInputArea(window,NULL,0);}
}
static inline int kl_window_input_scale(float *x,float *y,int sourceWidth,int sourceHeight,int windowWidth,int windowHeight){
 if(!x||!y||sourceWidth<1||sourceHeight<1||windowWidth<1||windowHeight<1)return 0;
 *x=klj_resolution_coordinate(*x,sourceWidth,windowWidth);*y=klj_resolution_coordinate(*y,sourceHeight,windowHeight);return 1;
}
static inline int kl_window_input_coordinates(SDL_Window *window,int sourceWidth,int sourceHeight,float *x,float *y){
 int width=0,height=0;
 return SDL_GetWindowSize(window,&width,&height)&&kl_window_input_scale(x,y,sourceWidth,sourceHeight,width,height);
}
static inline void kl_window_input_decode(const SDL_Event *event,SDL_WindowID owned,int textActive,KLWindowEvent *decoded){
 memset(decoded,0,sizeof(*decoded));if(!event)return;KLWindowInput *value=&decoded->value;
 if((event->type==SDL_EVENT_TEXT_INPUT&&event->text.windowID==owned)||(event->type==SDL_EVENT_TEXT_EDITING&&event->edit.windowID==owned)){
  int composing=event->type==SDL_EVENT_TEXT_EDITING;decoded->text=composing?event->edit.text:event->text.text;
  if(decoded->text){value->kind=KL_INPUT_TEXT;value->action=composing;decoded->start=composing?event->edit.start:0;decoded->length=composing?event->edit.length:0;}else value->kind=KL_INPUT_CONSUMED;
  return;
 }
 if((event->type==SDL_EVENT_KEY_DOWN||event->type==SDL_EVENT_KEY_UP)&&event->key.windowID==owned){
  const SDL_KeyboardEvent *key=&event->key;
  int command=(key->mod&SDL_KMOD_GUI)&&!(key->mod&(SDL_KMOD_CTRL|SDL_KMOD_ALT));
  if(command&&textActive&&(key->key==SDLK_C||key->key==SDLK_V||key->key==SDLK_A||key->key==SDLK_X)){
   value->kind=event->type==SDL_EVENT_KEY_DOWN&&!key->repeat?KL_INPUT_SHORTCUT:KL_INPUT_CONSUMED;
   value->code=29+(int)(key->key-SDLK_A);return;
  }
  /* The native text system owns printable text, dead keys and composition.
   * Navigation/physical game keys retain the original Android input route. */
  if(textActive&&key->key>=32&&key->key!=SDLK_DELETE&&key->key<SDLK_SCANCODE_MASK&&!(key->mod&(SDL_KMOD_CTRL|SDL_KMOD_GUI))){value->kind=KL_INPUT_CONSUMED;return;}
  value->code=klj_android_keycode(key->key);if(!value->code)return;
  value->kind=KL_INPUT_KEY;value->action=event->type==SDL_EVENT_KEY_DOWN?0:1;value->meta=klj_android_keymeta(key->mod);
  decoded->unicode=key->key>=32&&key->key<127?(int)key->key:0;return;
 }
 if(event->type==SDL_EVENT_QUIT||(event->type==SDL_EVENT_WINDOW_CLOSE_REQUESTED&&event->window.windowID==owned))value->kind=KL_INPUT_CLOSE;
 else if((event->type==SDL_EVENT_WINDOW_FOCUS_GAINED||event->type==SDL_EVENT_WINDOW_FOCUS_LOST)&&event->window.windowID==owned){value->kind=KL_INPUT_FOCUS;value->action=event->type==SDL_EVENT_WINDOW_FOCUS_GAINED;}
 else if((event->type==SDL_EVENT_MOUSE_BUTTON_DOWN||event->type==SDL_EVENT_MOUSE_BUTTON_UP)&&event->button.windowID==owned&&event->button.button==SDL_BUTTON_LEFT){value->kind=KL_INPUT_TOUCH;value->action=event->type==SDL_EVENT_MOUSE_BUTTON_DOWN?0:1;value->x=event->button.x;value->y=event->button.y;}
 else if(event->type==SDL_EVENT_MOUSE_MOTION&&event->motion.windowID==owned&&(event->motion.state&SDL_BUTTON_LMASK)){value->kind=KL_INPUT_TOUCH;value->action=2;value->x=event->motion.x;value->y=event->motion.y;}
 else if(event->type==SDL_EVENT_MOUSE_WHEEL&&event->wheel.windowID==owned){value->kind=KL_INPUT_WHEEL;value->x=event->wheel.mouse_x;value->y=event->wheel.mouse_y;value->dx=event->wheel.x;value->dy=event->wheel.y;}
}
