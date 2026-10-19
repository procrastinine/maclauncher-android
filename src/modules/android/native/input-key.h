#pragma once
#include <SDL3/SDL.h>
/* Physical Escape stays Escape. Cmd+[ is owned by the native Back menu. */
static int klj_android_keycode(SDL_Keycode key){
 if(key>=SDLK_A&&key<=SDLK_Z)return 29+(int)(key-SDLK_A);
 if(key>=SDLK_0&&key<=SDLK_9)return 7+(int)(key-SDLK_0);
 switch(key){case SDLK_UP:return 19;case SDLK_DOWN:return 20;case SDLK_LEFT:return 21;case SDLK_RIGHT:return 22;case SDLK_RETURN:return 66;case SDLK_ESCAPE:return 111;case SDLK_SPACE:return 62;case SDLK_BACKSPACE:return 67;case SDLK_DELETE:return 112;case SDLK_TAB:return 61;default:return 0;}
}
static int klj_android_keymeta(SDL_Keymod flags){return(flags&SDL_KMOD_SHIFT?1:0)|(flags&SDL_KMOD_ALT?2:0)|(flags&SDL_KMOD_CTRL?0x1000:0)|(flags&SDL_KMOD_GUI?0x10000:0);}
