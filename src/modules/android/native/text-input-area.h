#pragma once
#include <math.h>
#include <SDL3/SDL_rect.h>
/* The inverse of SDL pointer-to-Android mapping. SDL text geometry is measured
   in logical window points, independently of Retina and render resolution. */
static inline int klj_text_input_area(float left,float top,float right,float bottom,int sourceWidth,int sourceHeight,int windowWidth,int windowHeight,SDL_Rect*out){
 if(!out||sourceWidth<1||sourceHeight<1||windowWidth<1||windowHeight<1||!isfinite(left)||!isfinite(top)||!isfinite(right)||!isfinite(bottom)||right<left||bottom<top)return 0;
 float sx=(float)windowWidth/sourceWidth,sy=(float)windowHeight/sourceHeight;
 int x=(int)floorf(fmaxf(0,fminf(windowWidth,left*sx))),y=(int)floorf(fmaxf(0,fminf(windowHeight,top*sy)));
 int edgeX=(int)ceilf(fmaxf(x,fminf(windowWidth,right*sx))),edgeY=(int)ceilf(fmaxf(y,fminf(windowHeight,bottom*sy)));
 *out=(SDL_Rect){x,y,edgeX>x?edgeX-x:1,edgeY>y?edgeY-y:1};return 1;
}
