#pragma once
#include <stdint.h>
typedef struct KljXrSurface KljXrSurface;
KljXrSurface *klj_xr_surface_create(uint32_t width,uint32_t height);
void *klj_xr_surface_object(KljXrSurface *surface);
int klj_xr_surface_matches(KljXrSurface *surface,const void *native_window);
void klj_xr_surface_release(KljXrSurface *surface);
