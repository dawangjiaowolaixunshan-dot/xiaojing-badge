#include "pet.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "pet";
#define HEAD_MAX 65536

typedef struct __attribute__((packed)) {
    char name[24];
    uint16_t fps, nframes;
    uint32_t first;
} clip_hdr_t;

static const esp_partition_t *s_part;
static const uint8_t *s_head;           /* header + frame offset table, mapped for good */
static const clip_hdr_t *s_clips;
static const uint32_t *s_offsets;
static int s_size, s_nclips;
static bool s_paused;

typedef struct {
    lv_obj_t *img;
    lv_timer_t *timer;
    uint8_t *buf;                       /* the clip, read into PSRAM */
    uint32_t base_off;                  /* its offset in the partition */
    lv_image_dsc_t frames[2];           /* alternate, so LVGL sees a new source each step */
    uint8_t *px[2];                     /* their RGB565A8 pixels, converted from the 256-colour frame */
    pet_clip_t clip;
    int frame;
    bool loop, done;
} player_t;

/*
 * Read, not mapped: the partition reaches past the first 16 MB of flash, all
 * the cache can map on this chip; reads reach all 32 MB.
 */
bool pet_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "pet");
    uint8_t *head = heap_caps_malloc(HEAD_MAX, MALLOC_CAP_SPIRAM);
    if (!s_part || !head || esp_partition_read(s_part, 0, head, HEAD_MAX) != ESP_OK) {
        ESP_LOGE(TAG, "no pet partition: flash avatar/pet/pet.bin to it");
        free(head);
        return false;
    }
    if (memcmp(head, "PET1", 4)) {
        ESP_LOGE(TAG, "pet partition holds no clips");
        free(head);
        return false;
    }
    s_head = head;
    s_size = s_head[4] | s_head[5] << 8;
    s_nclips = s_head[6] | s_head[7] << 8;
    s_clips = (const clip_hdr_t *)(s_head + 8);
    s_offsets = (const uint32_t *)(s_head + 8 + s_nclips * sizeof(clip_hdr_t));
    if (s_nclips != PET_CLIP_COUNT) {
        ESP_LOGW(TAG, "partition has %d clips, firmware expects %d", s_nclips, PET_CLIP_COUNT);
    }
    ESP_LOGI(TAG, "%d clips of %d px", s_nclips, s_size);
    return true;
}

int pet_size(void) { return s_head ? s_size : 0; }

static void unmap(player_t *pl)
{
    free(pl->buf);
    pl->buf = NULL;
}

/*
 * Frames are stored as 256 colours (small enough to fit 19 clips), but LVGL
 * can't scale or rotate an indexed image. Each frame is converted once, when
 * it comes up, to RGB565 plus an alpha plane: drawable as is, and scalable.
 */
static void show_frame(player_t *pl)
{
    const clip_hdr_t *c = &s_clips[pl->clip];
    uint32_t off = s_offsets[c->first + pl->frame];
    const uint8_t *src = pl->buf + (off - pl->base_off);
    int slot = pl->frame & 1, n = s_size * s_size;
    if (!pl->px[slot]) {
        pl->px[slot] = heap_caps_malloc(n * 3, MALLOC_CAP_SPIRAM);
        if (!pl->px[slot]) return;
    }
    uint16_t lut[256];
    uint8_t alpha[256];
    for (int i = 0; i < 256; i++) {   /* palette entries are B, G, R, A */
        const uint8_t *e = src + 4 * i;
        lut[i] = (uint16_t)((e[2] >> 3) << 11 | (e[1] >> 2) << 5 | e[0] >> 3);
        alpha[i] = e[3];
    }
    const uint8_t *idx = src + 1024;
    uint16_t *rgb = (uint16_t *)pl->px[slot];
    uint8_t *a = pl->px[slot] + n * 2;
    for (int i = 0; i < n; i++) {
        rgb[i] = lut[idx[i]];
        a[i] = alpha[idx[i]];
    }
    lv_image_dsc_t *d = &pl->frames[slot];
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_RGB565A8;
    d->header.w = s_size;
    d->header.h = s_size;
    d->header.stride = s_size * 2;
    d->data_size = n * 3;
    d->data = pl->px[slot];
    lv_image_set_src(pl->img, d);
}

static void tick(lv_timer_t *t)
{
    player_t *pl = lv_timer_get_user_data(t);
    if (s_paused || !pl->buf || pl->done) return;
    int n = s_clips[pl->clip].nframes;
    if (pl->frame + 1 >= n) {
        if (!pl->loop) {
            pl->done = true;
            return;
        }
        pl->frame = -1;
    }
    pl->frame++;
    show_frame(pl);
}

static void on_delete(lv_event_t *e)
{
    player_t *pl = lv_event_get_user_data(e);
    lv_timer_delete(pl->timer);
    unmap(pl);
    free(pl->px[0]);
    free(pl->px[1]);
    lv_free(pl);
}

lv_obj_t *pet_create(lv_obj_t *parent)
{
    lv_obj_t *img = lv_image_create(parent);
    player_t *pl = lv_malloc_zeroed(sizeof(player_t));
    pl->img = img;
    pl->clip = PET_CLIP_COUNT;
    pl->timer = lv_timer_create(tick, 200, pl);
    lv_obj_set_user_data(img, pl);
    lv_obj_add_event_cb(img, on_delete, LV_EVENT_DELETE, pl);
    lv_obj_set_size(img, s_size ? s_size : 1, s_size ? s_size : 1);
    return img;
}

void pet_play(lv_obj_t *img, pet_clip_t clip, bool loop)
{
    player_t *pl = lv_obj_get_user_data(img);
    if (!s_head || !pl || clip >= s_nclips) return;
    if (clip == pl->clip && pl->buf) {   /* the same clip again: restart it */
        pl->loop = loop;
        pl->done = false;
        pl->frame = 0;
        show_frame(pl);
        return;
    }
    unmap(pl);
    const clip_hdr_t *c = &s_clips[clip];
    uint32_t first = s_offsets[c->first], last = s_offsets[c->first + c->nframes - 1];
    uint32_t len = last + 1024 + s_size * s_size - first;
    pl->buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!pl->buf) {
        ESP_LOGE(TAG, "no memory for clip %d (%u bytes)", clip, (unsigned)len);
        return;
    }
    esp_err_t err = esp_partition_read(s_part, first, pl->buf, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "can't read clip %d: %s", clip, esp_err_to_name(err));
        unmap(pl);
        return;
    }
    pl->base_off = first;
    pl->clip = clip;
    pl->loop = loop;
    pl->done = false;
    pl->frame = 0;
    lv_timer_set_period(pl->timer, 1000 / (c->fps ? c->fps : 5));
    show_frame(pl);
}

pet_clip_t pet_clip(lv_obj_t *img)
{
    player_t *pl = lv_obj_get_user_data(img);
    return pl ? pl->clip : PET_CLIP_COUNT;
}

bool pet_finished(lv_obj_t *img)
{
    player_t *pl = lv_obj_get_user_data(img);
    return !pl || pl->done;
}

void pet_set_paused(bool paused) { s_paused = paused; }

void pet_debug(void)
{
    lv_obj_t *scr = lv_screen_active();
    for (uint32_t i = 0; i < lv_obj_get_child_count(scr); i++) {
        lv_obj_t *o = lv_obj_get_child(scr, i);
        if (!lv_obj_check_type(o, &lv_image_class)) continue;
        player_t *pl = lv_obj_get_user_data(o);
        lv_area_t a;
        lv_obj_get_coords(o, &a);
        const void *src = lv_image_get_src(o);
        lv_image_header_t h = {0};
        lv_result_t r = src ? lv_image_decoder_get_info(src, &h) : LV_RESULT_INVALID;
        ESP_LOGI(TAG, "image %u: area %d,%d-%d,%d hidden=%d opa=%d pet=%d clip=%d frame=%d buf=%p info=%d cf=%d w=%d h=%d",
                 (unsigned)i, (int)a.x1, (int)a.y1, (int)a.x2, (int)a.y2, lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN),
                 lv_obj_get_style_opa(o, 0), pl != NULL && pl->img == o, pl && pl->img == o ? (int)pl->clip : -1,
                 pl && pl->img == o ? pl->frame : -1, pl && pl->img == o ? (void *)pl->buf : NULL, (int)r, (int)h.cf,
                 (int)h.w, (int)h.h);
    }
}
