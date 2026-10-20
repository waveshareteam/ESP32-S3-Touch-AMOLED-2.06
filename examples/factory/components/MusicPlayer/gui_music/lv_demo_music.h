/**
 * @file lv_demo_music.h
 *
 */

#ifndef APP_DEMO_MUSIC_H
#define APP_DEMO_MUSIC_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include "lvgl.h"
#include "bsp_board_extra.h"

#define APP_DEMO_MUSIC_ENABLE       1
#define APP_DEMO_MUSIC_LARGE        0
#define APP_DEMO_MUSIC_ROUND        (BSP_LCD_H_RES == 466 && BSP_LCD_V_RES == 466)
/* Upstream's SQUARE mode deliberately splits the cover and controls over two
 * pages.  The 466 px round product instead uses a compact one-page player, so
 * keep SQUARE for genuinely square, non-round targets only. */
#define APP_DEMO_MUSIC_SQUARE       (BSP_LCD_H_RES == BSP_LCD_V_RES && !APP_DEMO_MUSIC_ROUND)
#define APP_DEMO_MUSIC_LANDSCAPE    (BSP_LCD_H_RES > BSP_LCD_V_RES)

#if APP_DEMO_MUSIC_ENABLE

/*********************
 *      DEFINES
 *********************/

#if APP_DEMO_MUSIC_LARGE
#  define APP_DEMO_MUSIC_HANDLE_SIZE  40
#else
#  define APP_DEMO_MUSIC_HANDLE_SIZE  20
#endif

/* 466 px circular panel geometry.  A 326 px square centred at (233, 233)
 * stays inside the visible circle, including its four corners.  Keep these
 * values explicit: the round layout is deliberately composed for this panel,
 * rather than obtained by scaling the rectangular demo. */
#define APP_DEMO_MUSIC_ROUND_SAFE_MARGIN    70
#define APP_DEMO_MUSIC_ROUND_SAFE_SIZE      (BSP_LCD_H_RES - (2 * APP_DEMO_MUSIC_ROUND_SAFE_MARGIN))
#define APP_DEMO_MUSIC_ROUND_DETAIL_TOP     70
#define APP_DEMO_MUSIC_ROUND_DETAIL_BOTTOM  70
#define APP_DEMO_MUSIC_ROUND_PROGRESS_WIDTH 220
#define APP_DEMO_MUSIC_ROUND_TITLE_WIDTH    220
#define APP_DEMO_MUSIC_ROUND_TITLE_Y        30
#define APP_DEMO_MUSIC_ROUND_SPECTRUM_Y     88
#define APP_DEMO_MUSIC_ROUND_SPECTRUM_HEIGHT 230
#define APP_DEMO_MUSIC_ROUND_ICONS_WIDTH    230
#define APP_DEMO_MUSIC_ROUND_ICONS_Y        90
#define APP_DEMO_MUSIC_ROUND_CONTROLS_Y     299
#define APP_DEMO_MUSIC_ROUND_HANDLE_WIDTH   160
#define APP_DEMO_MUSIC_ROUND_HANDLE_Y       416

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 * GLOBAL PROTOTYPES
 **********************/

void lv_demo_music(lv_obj_t *parent, file_iterator_instance_t *file_iterator);
void lv_demo_music_close(void);

const char * lv_demo_music_get_title(uint32_t track_id);
uint32_t lv_demo_music_get_track_count(void);
const char * lv_demo_music_get_artist(uint32_t track_id);
const char * lv_demo_music_get_genre(uint32_t track_id);
uint32_t lv_demo_music_get_track_length(uint32_t track_id);

/**********************
 *      MACROS
 **********************/

#endif /*APP_DEMO_MUSIC_ENABLE*/

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /*APP_DEMO_MUSIC_H*/
