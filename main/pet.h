#pragma once
#include <stdbool.h>

#include "lvgl.h"
#include "pet_clips.h"

/*
 * The whale girl, animated: clips from dsh-pet (MIT), stored in the "pet"
 * flash partition as 256-colour frames and read in place. A pet is an
 * lv_image that plays one clip at a time; only that clip's frames are mapped.
 */
bool pet_init(void);
int pet_size(void);                                 /* frame width = height, px */
lv_obj_t *pet_create(lv_obj_t *parent);
void pet_play(lv_obj_t *pet, pet_clip_t clip, bool loop);
pet_clip_t pet_clip(lv_obj_t *pet);
bool pet_finished(lv_obj_t *pet);                   /* a one-shot clip reached its end */
void pet_set_paused(bool paused);                   /* all pets: while the screen is off */
void pet_debug(void);   /* log the screen's images (over USB: >petdbg) */
