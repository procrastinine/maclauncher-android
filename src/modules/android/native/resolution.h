#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
/* Source allocation, never a post-render downsample. Integer arithmetic keeps
   odd output extents inside the scaler's advertised maximum ratio. */
static inline int klj_resolution_ratio(const char *method,const char *value,int *numerator,int *denominator){
 *numerator=1;*denominator=1;
 if(!value||!*value)return 1;
 if(!method||!*method||strcmp(method,"off")==0)return 0;
 if(strcmp(value,"1")==0)return 1;
 if(strcmp(value,"1.5")==0){*numerator=3;*denominator=2;return 1;}
 if(strcmp(value,"2")==0){*numerator=2;return 1;}
 return 0;
}
static inline int klj_resolution_dimension(int output,int numerator,int denominator){
 return output>0&&output<=8192&&numerator>0&&denominator>0?(output*denominator+numerator-1)/numerator:0;
}
static inline uint64_t klj_resolution_pack(int width,int height,int outputWidth,int outputHeight){
 return (uint64_t)(uint16_t)width<<48|(uint64_t)(uint16_t)height<<32|(uint64_t)(uint16_t)outputWidth<<16|(uint16_t)outputHeight;
}
static inline float klj_resolution_coordinate(float point,int source,int window){return window>0?point*(float)source/(float)window:0;}
