#pragma once
#include <SDL3/SDL.h>
struct KlDesktopXrInput {
 float px=0,py=1.6f,pz=0,yaw=0,pitch=0;
 int mouse_l=0,mouse_r=0,mouselook=0;
 uint64_t previous=0;
 bool acceptedKeys[SDL_SCANCODE_COUNT]={};
 void event(SDL_Window*,const SDL_Event&,bool appHandled);
 void tick(SDL_Window*,bool enabled);
 void release(SDL_Window*);
};
