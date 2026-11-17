#pragma once
#include <stdbool.h>

#include "lvgl.h"

/* Colours (RGB888) on the black AMOLED. */
#define C_TEXT 0xf2e8d5
#define C_DIM 0x8c8577
#define C_ACCENT 0xe08a45
#define C_BLUE 0x4d6bfe
#define C_GREEN 0x4caf7d
#define C_RED 0xd9534f
#define C_PANEL 0x1d1a17

extern lv_font_t g_font_text;   /* 22 px, GB2312 + the poems' characters */
extern lv_font_t g_font_poem;   /* 32 px, only the poems' characters */

void ui_start(void);
bool ui_lock(void);
void ui_unlock(void);
void ui_settings_changed(void);
void ui_boot_button(bool pressed);   /* BOOT: back, on release */
void ui_talk_button(bool pressed);   /* PWR: hold to talk where a screen talks */

/* Helpers for the screens (call with the lock held). */
lv_obj_t *ui_screen(void);
void ui_show(lv_obj_t *scr, void (*back)(void), void (*talk)(bool pressed));
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text);
lv_obj_t *ui_button(lv_obj_t *parent, const char *text, const lv_font_t *font, int w, int h, uint32_t bg,
                    lv_event_cb_t cb, void *user);
void ui_back_button(lv_obj_t *scr);
void ui_home(void);

/* Screens. */
void ui_poem_home(void);
void ui_chat(void);
void ui_chat_inject(const char *question);   /* as if heard (testing over USB) */
