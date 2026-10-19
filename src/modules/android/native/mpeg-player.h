// Checked access to the existing Klepton MPEG audio player. No codec lives here.
#ifndef KLJ_MPEG_PLAYER_H
#define KLJ_MPEG_PLAYER_H
#include <stddef.h>
#include "kl_mediaplayer.h"
int klj_mpeg_ready(kl_mediaplayer *player);
/* Borrows the validated bytes for this call; indexed audio remains owned. */
int klj_mpeg_prepare_bytes(kl_mediaplayer *player,const void *bytes,size_t size);
int klj_mpeg_error(kl_mediaplayer *player);
int klj_mpeg_start(kl_mediaplayer *player);
int klj_mpeg_volume(kl_mediaplayer *player,float left,float right);
size_t klj_mpeg_allocated_bytes(void);
#endif
