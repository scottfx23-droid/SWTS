/*
 * LVGL Configuration for SWTS Datapad
 * ESP32-S3 + ST7796 320x480
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH     16
#define LV_COLOR_16_SWAP   1

#define LV_MEM_CUSTOM      1
#define LV_MEM_CUSTOM_INCLUDE   <stdlib.h>
#define LV_MEM_CUSTOM_ALLOC     malloc
#define LV_MEM_CUSTOM_FREE      free
#define LV_MEM_CUSTOM_REALLOC   realloc

#define LV_TICK_CUSTOM      1
#define LV_TICK_CUSTOM_INCLUDE  "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

#define LV_DPI_DEF          130

/* Drawing */
#define LV_DRAW_COMPLEX     1
#define LV_SHADOW_CACHE_SIZE 0
#define LV_CIRCLE_CACHE_SIZE 4

/* Fonts — built-in */
#define LV_FONT_MONTSERRAT_12  1
#define LV_FONT_MONTSERRAT_14  1
#define LV_FONT_MONTSERRAT_16  1
#define LV_FONT_MONTSERRAT_18  1
#define LV_FONT_MONTSERRAT_20  1
#define LV_FONT_MONTSERRAT_24  1
#define LV_FONT_MONTSERRAT_28  1
#define LV_FONT_MONTSERRAT_36  1
#define LV_FONT_DEFAULT        &lv_font_montserrat_14

/* Features */
#define LV_USE_ANIMIMG     0
#define LV_USE_ARC         1
#define LV_USE_BAR         1
#define LV_USE_BTN         1
#define LV_USE_BTNMATRIX   1
#define LV_USE_CANVAS      0
#define LV_USE_CHECKBOX    0
#define LV_USE_DROPDOWN    0
#define LV_USE_IMG         1
#define LV_USE_LABEL       1
#define LV_USE_LINE        1
#define LV_USE_ROLLER      0
#define LV_USE_SLIDER      0
#define LV_USE_SWITCH      0
#define LV_USE_TEXTAREA    1
#define LV_USE_TABLE       0
#define LV_USE_TABVIEW     1
#define LV_USE_TILEVIEW    0
#define LV_USE_WIN         0
#define LV_USE_SPAN        0
#define LV_USE_SPINBOX     0
#define LV_USE_SPINNER     1
#define LV_USE_LIST        1
#define LV_USE_METER       1
#define LV_USE_MSGBOX      1
#define LV_USE_CHART       0
#define LV_USE_CALENDAR    0
#define LV_USE_COLORWHEEL  0
#define LV_USE_LED         1
#define LV_USE_KEYBOARD    1
#define LV_USE_MENU        0

/* Animation */
#define LV_USE_ANIM        1

/* Logging — disable for production */
#define LV_USE_LOG         0

/* Themes */
#define LV_USE_THEME_DEFAULT   1
#define LV_THEME_DEFAULT_DARK  1

#endif /* LV_CONF_H */
