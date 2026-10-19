#pragma once
#include <stdint.h>

/* SDL desktop coordinates and window sizes are logical points. Decorations
   consume work-area space, but never affect the renderer's pixel dimensions. */
typedef struct { int x,y,width,height; } KLWindowPlacement;
static inline KLWindowPlacement kl_window_placement(int width,int height,
 int x,int y,int availableWidth,int availableHeight,int top,int left,int bottom,int right) {
 KLWindowPlacement result={x+left,y+top,width,height};
 int64_t maxWidth=(int64_t)availableWidth-left-right;
 int64_t maxHeight=(int64_t)availableHeight-top-bottom;
 if(width<1||height<1||maxWidth<1||maxHeight<1)return result;
 if(width>maxWidth||height>maxHeight){
  if(maxWidth*height<=maxHeight*width){result.width=(int)maxWidth;result.height=(int)((int64_t)height*maxWidth/width);}
  else{result.height=(int)maxHeight;result.width=(int)((int64_t)width*maxHeight/height);}
  if(result.width<1)result.width=1;if(result.height<1)result.height=1;
 }
 result.x+=(int)((maxWidth-result.width)/2);
 result.y+=(int)((maxHeight-result.height)/2);
 return result;
}
