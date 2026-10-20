/**
 * @file lv_demo_music_main.h
 *
 */

#ifndef APP_DEMO_MUSIC_MAIN_H
#define APP_DEMO_MUSIC_MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include "lv_demo_music.h"
#include "bsp_board_extra.h"
#if APP_DEMO_MUSIC_ENABLE

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 * GLOBAL PROTOTYPES
 **********************/
lv_obj_t * lv_demo_music_main_create(lv_obj_t * parent, file_iterator_instance_t *iterator);
void lv_demo_music_main_close(void);
void lv_demo_music_play(uint32_t id);
void lv_demo_music_resume(void);
void lv_demo_music_pause(void);
void lv_demo_music_exit_pause(void);
void lv_demo_music_album_next(bool next);

/**********************
 *      MACROS
 **********************/
#endif /*LV_USE_DEMO_MUSIC*/

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /*LV_DEMO_MUSIC_MAIN_H*/
