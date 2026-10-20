/**
 * @file lv_demo_music_main.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "lv_demo_music_main.h"
#if APP_DEMO_MUSIC_ENABLE

#include "lv_demo_music_list.h"
#include "assets/spectrum_1.h"
#include "assets/spectrum_2.h"
#include "assets/spectrum_3.h"

#include "lv_demo_music.h"
#include "esp_log.h"
#include "bsp_board_extra.h"
#include "audio_player.h"

/*********************
 *      DEFINES
 *********************/
#define INTRO_TIME          2000
#define MUSIC_COMPACT_TOUCH_CONTROLS (BSP_LCD_H_RES == 410 && BSP_LCD_V_RES == 502 && !APP_DEMO_MUSIC_LARGE)
#if MUSIC_COMPACT_TOUCH_CONTROLS
    /* Reserve room for the enlarged button without pushing controls below the panel. */
    #define MUSIC_SPECTRUM_HEIGHT 230
    #define MUSIC_SPECTRUM_REFRESH_PERIOD_MS 50
#else
    #define MUSIC_SPECTRUM_HEIGHT 250
#endif
#define BAR_COLOR1          lv_color_hex(0xe9dbfc)
#define BAR_COLOR2          lv_color_hex(0x6f8af6)
#define BAR_COLOR3          lv_color_hex(0xffffff)
#if APP_DEMO_MUSIC_LARGE
    #define BAR_COLOR1_STOP     160
    #define BAR_COLOR2_STOP     200
    #define BAR_REST_RADIUS     165
#else
    #define BAR_COLOR1_STOP     80
    #define BAR_COLOR2_STOP     100
    #define BAR_REST_RADIUS     82
#endif
#define BAR_COLOR3_STOP     (LV_MAX(LV_HOR_RES, LV_VER_RES) / 3)
#define BAR_CNT             20
#define DEG_STEP            (180/BAR_CNT)
#define BAND_CNT            4
#define BAR_PER_BAND_CNT    (BAR_CNT / BAND_CNT)

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static lv_obj_t * create_cont(lv_obj_t * parent);
static void create_wave_images(lv_obj_t * parent);
static lv_obj_t * create_title_box(lv_obj_t * parent);
static lv_obj_t * create_icon_box(lv_obj_t * parent);
static lv_obj_t * create_spectrum_obj(lv_obj_t * parent);
static lv_obj_t * create_ctrl_box(lv_obj_t * parent);
static lv_obj_t * create_handle(lv_obj_t * parent);

static void spectrum_anim_cb(void * a, int32_t v);
static void start_anim_cb(void * var, int32_t v);
static void del_counter_timer_cb(lv_event_t * e);
static void spectrum_draw_event_cb(lv_event_t * e);
static lv_obj_t * album_image_create(lv_obj_t * parent);
static void album_gesture_event_cb(lv_event_t * e);
static void play_event_press_cb(lv_event_t * e);
static bool spectrum_refresh_ready(void);
static void prev_click_event_cb(lv_event_t * e);
static void next_click_event_cb(lv_event_t * e);
static void timer_cb(lv_timer_t * t);
static void track_load(uint32_t id);
static void stop_start_anim(lv_timer_t * t);
static void spectrum_end_cb(lv_anim_t * a);
static void album_fade_anim_cb(void * var, int32_t v);
static int32_t get_cos(int32_t deg, int32_t a);
static int32_t get_sin(int32_t deg, int32_t a);

/**********************
 *  STATIC VARIABLES
 **********************/
static lv_obj_t * main_cont;
static lv_obj_t * spectrum_obj;
static lv_obj_t * title_label;
static lv_obj_t * artist_label;
static lv_obj_t * genre_label;
static lv_obj_t * time_obj;
static lv_obj_t * album_image_obj;
static lv_obj_t * slider_obj;
static uint32_t spectrum_i = 0;
static uint32_t spectrum_i_pause = 0;
static uint32_t bar_ofs = 0;
static uint32_t spectrum_lane_ofs_start = 0;
static uint32_t bar_rot = 0;
static uint32_t time_act;
static lv_timer_t  * stop_start_anim_timer;
static lv_timer_t  * sec_counter_timer;
static const lv_font_t * font_small;
static const lv_font_t * font_large;
static uint32_t track_id;
static bool playing;
static bool start_anim;
static int32_t start_anim_values[40];
static lv_obj_t * play_obj;
static const uint16_t (* spectrum)[4];
static uint32_t spectrum_len;
static uint32_t last_spectrum_draw_tick;
static const uint16_t rnd_array[30] = {994, 285, 553, 11, 792, 707, 966, 641, 852, 827, 44, 352, 146, 581, 490, 80, 729, 58, 695, 940, 724, 561, 124, 653, 27, 292, 557, 506, 382, 199};

static file_iterator_instance_t *file_iterator;
static bool pause = false;
static bool pause_exit = false;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/*
 * Callback adapter function to convert parameter types to avoid compile-time
 * warning.
 */
static void _image_set_scale_anim_cb(void * obj, int32_t scale)
{
    lv_image_set_scale((lv_obj_t *)obj, (uint16_t)scale);
}

/*
 * Callback adapter function to convert parameter types to avoid compile-time
 * warning.
 */
static void _obj_set_x_anim_cb(void * obj, int32_t x)
{
    lv_obj_set_x((lv_obj_t *)obj, (int32_t)x);
}

lv_obj_t * lv_demo_music_main_create(lv_obj_t * parent, file_iterator_instance_t *iterator)
{
    file_iterator = iterator;
    pause = false;
    pause_exit = false;
    playing = false;
    spectrum_i = 0;
    spectrum_i_pause = 0;
    bar_ofs = 0;
    spectrum_lane_ofs_start = 0;
    bar_rot = 0;
    time_act = 0;
    track_id = 0;
    start_anim = false;
    spectrum_len = 0;
    last_spectrum_draw_tick = 0;

#if APP_DEMO_MUSIC_LARGE
    font_small = &lv_font_montserrat_22;
    font_large = &lv_font_montserrat_32;
#else
    font_small = &lv_font_montserrat_12;
    font_large = &lv_font_montserrat_16;
#endif

    /*Create the content of the music player*/
    lv_obj_t * cont = create_cont(parent);

    create_wave_images(cont);
    lv_obj_t * title_box = create_title_box(cont);
    lv_obj_t * icon_box = create_icon_box(cont);
    lv_obj_t * ctrl_box = create_ctrl_box(cont);
    spectrum_obj = create_spectrum_obj(cont);
    lv_obj_t * handle_box = create_handle(cont);

    /*Arrange the content into a grid*/
#if APP_DEMO_MUSIC_ROUND
    /* The copied reference leaves its adaptive macros at zero, therefore its
     * normal product UI is a one-page portrait player.  Do the same here with
     * explicit round geometry instead of selecting upstream SQUARE mode (which
     * puts the controls on a second page).  Child Y values include the 20 px
     * player pre-roll because player itself starts at y=-20. */
    lv_obj_set_width(title_box, APP_DEMO_MUSIC_ROUND_TITLE_WIDTH);
    lv_obj_set_pos(title_box,
                   (BSP_LCD_H_RES - APP_DEMO_MUSIC_ROUND_TITLE_WIDTH) / 2,
                   APP_DEMO_MUSIC_HANDLE_SIZE + APP_DEMO_MUSIC_ROUND_TITLE_Y);

    lv_obj_set_size(spectrum_obj, BSP_LCD_H_RES, APP_DEMO_MUSIC_ROUND_SPECTRUM_HEIGHT);
    lv_obj_set_pos(spectrum_obj, 0,
                   APP_DEMO_MUSIC_HANDLE_SIZE + APP_DEMO_MUSIC_ROUND_SPECTRUM_Y);
    /* Spectrum triangles can extend beyond their object's coordinates. Keep
     * them behind the labels and controls while the album remains centred. */
    lv_obj_move_background(spectrum_obj);

    lv_obj_set_width(icon_box, APP_DEMO_MUSIC_ROUND_ICONS_WIDTH);
    lv_obj_set_pos(icon_box,
                   (BSP_LCD_H_RES - APP_DEMO_MUSIC_ROUND_ICONS_WIDTH) / 2,
                   APP_DEMO_MUSIC_HANDLE_SIZE + APP_DEMO_MUSIC_ROUND_ICONS_Y);

    lv_obj_set_width(ctrl_box, APP_DEMO_MUSIC_ROUND_SAFE_SIZE);
    lv_obj_set_pos(ctrl_box, APP_DEMO_MUSIC_ROUND_SAFE_MARGIN,
                   APP_DEMO_MUSIC_HANDLE_SIZE + APP_DEMO_MUSIC_ROUND_CONTROLS_Y);

    lv_obj_set_width(handle_box, APP_DEMO_MUSIC_ROUND_HANDLE_WIDTH);
    lv_obj_set_pos(handle_box,
                   (BSP_LCD_H_RES - APP_DEMO_MUSIC_ROUND_HANDLE_WIDTH) / 2,
                   APP_DEMO_MUSIC_HANDLE_SIZE + APP_DEMO_MUSIC_ROUND_HANDLE_Y);
#elif APP_DEMO_MUSIC_SQUARE
    static const int32_t grid_cols[] = {LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static int32_t grid_rows[] = {APP_DEMO_MUSIC_HANDLE_SIZE,     /*Spacing*/
                                  0,   /*Spectrum obj, set later*/
                                  LV_GRID_CONTENT, /*Title box*/
                                  LV_GRID_FR(3),   /*Spacer*/
                                  LV_GRID_CONTENT, /*Icon box*/
                                  LV_GRID_FR(3),   /*Spacer*/
                                  LV_GRID_CONTENT, /*Control box*/
                                  LV_GRID_FR(3),   /*Spacer*/
                                  LV_GRID_CONTENT, /*Handle box*/
                                  APP_DEMO_MUSIC_HANDLE_SIZE,     /*Spacing*/
                                  LV_GRID_TEMPLATE_LAST
                                 };

    grid_rows[1] = LV_VER_RES;

    lv_obj_set_grid_dsc_array(cont, grid_cols, grid_rows);
    lv_obj_set_style_grid_row_align(cont, LV_GRID_ALIGN_SPACE_BETWEEN, 0);
    lv_obj_set_grid_cell(spectrum_obj, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_CENTER, 1, 1);
    lv_obj_set_grid_cell(title_box, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_CENTER, 2, 1);
    lv_obj_set_grid_cell(icon_box, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_CENTER, 4, 1);
    lv_obj_set_grid_cell(ctrl_box, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_CENTER, 6, 1);
    lv_obj_set_grid_cell(handle_box, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_CENTER, 8, 1);
#elif APP_DEMO_MUSIC_LANDSCAPE == 0
    static const int32_t grid_cols[] = {LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static const int32_t grid_rows[] = {APP_DEMO_MUSIC_HANDLE_SIZE,     /*Spacing*/
                                        LV_GRID_FR(1),   /*Spacer*/
                                        LV_GRID_CONTENT, /*Title box*/
                                        LV_GRID_FR(3),   /*Spacer*/
                                        LV_GRID_CONTENT, /*Icon box*/
                                        LV_GRID_FR(3),   /*Spacer*/
# if APP_DEMO_MUSIC_LARGE == 0
                                        MUSIC_SPECTRUM_HEIGHT, /*Spectrum obj*/
# else
                                        480,   /*Spectrum obj*/
# endif
                                        LV_GRID_FR(3),   /*Spacer*/
                                        LV_GRID_CONTENT, /*Control box*/
                                        LV_GRID_FR(3),   /*Spacer*/
                                        LV_GRID_CONTENT, /*Handle box*/
                                        LV_GRID_FR(1),   /*Spacer*/
                                        APP_DEMO_MUSIC_HANDLE_SIZE,     /*Spacing*/
                                        LV_GRID_TEMPLATE_LAST
                                       };

    lv_obj_set_grid_dsc_array(cont, grid_cols, grid_rows);
    lv_obj_set_style_grid_row_align(cont, LV_GRID_ALIGN_SPACE_BETWEEN, 0);
    lv_obj_set_grid_cell(title_box, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_CENTER, 2, 1);
    lv_obj_set_grid_cell(icon_box, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_CENTER, 4, 1);
    lv_obj_set_grid_cell(spectrum_obj, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_CENTER, 6, 1);
    lv_obj_set_grid_cell(ctrl_box, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_CENTER, 8, 1);
    lv_obj_set_grid_cell(handle_box, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_CENTER, 10, 1);
#else
    /*Arrange the content into a grid*/
    static const int32_t grid_cols[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static const int32_t grid_rows[] = {APP_DEMO_MUSIC_HANDLE_SIZE,     /*Spacing*/
                                        LV_GRID_FR(1),   /*Spacer*/
                                        LV_GRID_CONTENT, /*Title box*/
                                        LV_GRID_FR(1),   /*Spacer*/
                                        LV_GRID_CONTENT, /*Icon box*/
                                        LV_GRID_FR(3),   /*Spacer*/
                                        LV_GRID_CONTENT, /*Control box*/
                                        LV_GRID_FR(1),   /*Spacer*/
                                        LV_GRID_CONTENT, /*Handle box*/
                                        LV_GRID_FR(1),   /*Spacer*/
                                        APP_DEMO_MUSIC_HANDLE_SIZE,     /*Spacing*/
                                        LV_GRID_TEMPLATE_LAST
                                       };

    lv_obj_set_grid_dsc_array(cont, grid_cols, grid_rows);
    lv_obj_set_style_grid_row_align(cont, LV_GRID_ALIGN_SPACE_BETWEEN, 0);
    lv_obj_set_grid_cell(title_box, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_CENTER, 2, 1);
    lv_obj_set_grid_cell(icon_box, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_CENTER, 4, 1);
    lv_obj_set_grid_cell(ctrl_box, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_CENTER, 6, 1);
    lv_obj_set_grid_cell(handle_box, LV_GRID_ALIGN_STRETCH, 0, 2, LV_GRID_ALIGN_CENTER, 8, 1);
    lv_obj_set_grid_cell(spectrum_obj, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_CENTER, 1, 9);
#endif

    sec_counter_timer = lv_timer_create(timer_cb, 1000, NULL);
    if(sec_counter_timer) {
        lv_timer_pause(sec_counter_timer);
    }

    /* Animate in the content exactly as the reference MusicPlayer does. */
    lv_anim_t a;

    start_anim = true;

    stop_start_anim_timer = lv_timer_create(stop_start_anim, INTRO_TIME + 3000, NULL);
    if(stop_start_anim_timer) {
        lv_timer_set_repeat_count(stop_start_anim_timer, 1);
    }
    else {
        /* Do not leave the renderer in its intro-only state if the timer
         * allocation fails under memory pressure. */
        start_anim = false;
    }

    lv_anim_init(&a);

    uint32_t i;
    lv_anim_set_exec_cb(&a, start_anim_cb);
    lv_anim_set_values(&a, LV_MAX(LV_HOR_RES, LV_VER_RES) / 2, 0);
    lv_anim_set_path_cb(&a, lv_anim_path_bounce);
    for(i = 0; i < BAR_CNT; i++) {
        lv_anim_set_delay(&a, INTRO_TIME - 200 + (rnd_array[i] % 200));
        lv_anim_set_duration(&a, 2500 + (rnd_array[i] % 500));
        lv_anim_set_var(&a, &start_anim_values[i]);
        lv_anim_start(&a);
    }

    lv_obj_fade_in(title_box, 1000, INTRO_TIME + 1000);
    lv_obj_fade_in(icon_box, 1000, INTRO_TIME + 1000);
    lv_obj_fade_in(ctrl_box, 1000, INTRO_TIME + 1000);
    lv_obj_fade_in(handle_box, 1000, INTRO_TIME + 1000);
    lv_obj_fade_in(album_image_obj, 800, INTRO_TIME + 1000);
    lv_obj_fade_in(spectrum_obj, 0, INTRO_TIME);

    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_var(&a, album_image_obj);
    lv_anim_set_duration(&a, 1000);
    lv_anim_set_delay(&a, INTRO_TIME + 1000);
    lv_anim_set_values(&a, 1, LV_SCALE_NONE);
    lv_anim_set_exec_cb(&a, _image_set_scale_anim_cb);
    lv_anim_set_completed_cb(&a, NULL);
    lv_anim_start(&a);

    /* Create the reference intro logo inside this application's parent so
     * Brookesia can track and clean it with the rest of the demo. */
    LV_IMAGE_DECLARE(img_lv_demo_music_logo);
    lv_obj_t * logo = lv_image_create(parent);
    lv_image_set_src(logo, &img_lv_demo_music_logo);
    lv_obj_move_foreground(logo);
    lv_obj_align_to(logo, spectrum_obj, LV_ALIGN_CENTER, 0, 0);

#if APP_DEMO_MUSIC_SQUARE == 0 && APP_DEMO_MUSIC_ROUND == 0
    lv_obj_t * title = lv_label_create(parent);
    lv_label_set_text(title, "LVGL Demo\nMusic player");
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(title, font_large, 0);
    lv_obj_set_style_text_line_space(title, 8, 0);
    lv_obj_fade_out(title, 500, INTRO_TIME);
    lv_obj_align_to(title, logo, LV_ALIGN_OUT_LEFT_MID, -20, 0);
#endif

    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_var(&a, logo);
    lv_anim_set_duration(&a, 400);
    lv_anim_set_delay(&a, INTRO_TIME + 800);
    lv_anim_set_values(&a, LV_SCALE_NONE, 10);
    lv_anim_set_completed_cb(&a, lv_obj_delete_anim_completed_cb);
    lv_anim_start(&a);

    lv_obj_update_layout(main_cont);

    return main_cont;
}

void lv_demo_music_main_close(void)
{
    if(stop_start_anim_timer) {
        lv_timer_del(stop_start_anim_timer);
        stop_start_anim_timer = NULL;
    }
    if(sec_counter_timer) {
        lv_timer_del(sec_counter_timer);
        sec_counter_timer = NULL;
    }
}


void lv_demo_music_album_next(bool next)
{
    uint32_t track_count = lv_demo_music_get_track_count();
    if(track_count == 0) {
        return;
    }

    uint32_t id = track_id;
    if(next) {
        id++;
        if(id >= track_count) id = 0;
    }
    else {
        if(id == 0) {
            id = track_count - 1;
        }
        else {
            id--;
        }
    }

    if(playing) {
        lv_demo_music_play(id);
    }
    else {
        track_load(id);
    }
}

void lv_demo_music_play(uint32_t id)
{
    if(id >= lv_demo_music_get_track_count()) {
        return;
    }

    track_load(id);

    lv_demo_music_resume();
}

void lv_demo_music_resume(void)
{
    if(track_id >= lv_demo_music_get_track_count() || spectrum_len == 0) {
        return;
    }

    esp_err_t ret;
    if (!pause_exit && pause && bsp_extra_player_is_playing_by_index(file_iterator, track_id)) {
        LV_LOG_USER("Resume music");
        ret = audio_player_resume();
    }
    else {
        pause_exit = false;
        LV_LOG_USER("Music is not playing. Start playing.");
        ret = bsp_extra_player_play_index(file_iterator, track_id);
    }

    if(ret != ESP_OK) {
        ESP_LOGE("MusicPlayer", "Failed to start track %" LV_PRIu32 ": %s",
                 track_id, esp_err_to_name(ret));
        lv_demo_music_pause();
        pause = false;
        spectrum_i_pause = 0;
        time_act = 0;
        lv_slider_set_value(slider_obj, 0, LV_ANIM_OFF);
        lv_label_set_text(time_obj, "0:00");
        return;
    }

    spectrum_i = spectrum_i_pause;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_values(&a, spectrum_i, spectrum_len - 1);
    lv_anim_set_exec_cb(&a, spectrum_anim_cb);
    lv_anim_set_var(&a, spectrum_obj);
    lv_anim_set_duration(&a, ((spectrum_len - spectrum_i) * 1000) / 30);
    lv_anim_set_playback_duration(&a, 0);
    lv_anim_set_completed_cb(&a, spectrum_end_cb);
    lv_anim_start(&a);

    if(sec_counter_timer) lv_timer_resume(sec_counter_timer);
    lv_slider_set_range(slider_obj, 0, lv_demo_music_get_track_length(track_id));

    lv_obj_add_state(play_obj, LV_STATE_CHECKED);
    playing = true;
    pause = false;
}

void lv_demo_music_pause(void)
{
    if(!playing || lv_demo_music_get_track_count() == 0) {
        return;
    }

    /* Submit the non-blocking audio command before changing the visible state. */
    esp_err_t ret = audio_player_pause();
    if(ret != ESP_OK) {
        ESP_LOGE("MusicPlayer", "Failed to pause track %" LV_PRIu32 ": %s",
                 track_id, esp_err_to_name(ret));
        return;
    }

    playing = false;
    pause = true;
    spectrum_i_pause = spectrum_i;
    spectrum_i = 0;
    lv_anim_delete(spectrum_obj, spectrum_anim_cb);
    lv_obj_invalidate(spectrum_obj);
    lv_image_set_scale(album_image_obj, LV_SCALE_NONE);
    if(sec_counter_timer) lv_timer_pause(sec_counter_timer);
    lv_obj_remove_state(play_obj, LV_STATE_CHECKED);

}

void lv_demo_music_exit_pause(void)
{
    pause_exit = true;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static lv_obj_t * create_cont(lv_obj_t * parent)
{
    /*A transparent container in which the player section will be scrolled*/
    main_cont = lv_obj_create(parent);
    lv_obj_remove_flag(main_cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(main_cont, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_remove_style_all(main_cont);                            /*Make it transparent*/
    lv_obj_set_size(main_cont, lv_pct(100), lv_pct(100));
    lv_obj_set_scroll_snap_y(main_cont, LV_SCROLL_SNAP_CENTER);    /*Snap the children to the center*/

    /*Create a container for the player*/
    lv_obj_t * player = lv_obj_create(main_cont);
    lv_obj_set_y(player, - APP_DEMO_MUSIC_HANDLE_SIZE);
#if APP_DEMO_MUSIC_SQUARE
    lv_obj_set_size(player, LV_HOR_RES, 2 * LV_VER_RES + APP_DEMO_MUSIC_HANDLE_SIZE * 2);
#else
    lv_obj_set_size(player, LV_HOR_RES, LV_VER_RES + APP_DEMO_MUSIC_HANDLE_SIZE * 2);
#endif
    lv_obj_remove_flag(player, LV_OBJ_FLAG_SNAPPABLE);

    lv_obj_set_style_bg_color(player, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_width(player, 0, 0);
    lv_obj_set_style_pad_all(player, 0, 0);
    lv_obj_set_scroll_dir(player, LV_DIR_VER);

    /* Transparent placeholders below the player container
     * It is used only to snap it to center.*/
    lv_obj_t * placeholder1 = lv_obj_create(main_cont);
    lv_obj_remove_style_all(placeholder1);
    lv_obj_remove_flag(placeholder1, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * placeholder2 = lv_obj_create(main_cont);
    lv_obj_remove_style_all(placeholder2);
    lv_obj_remove_flag(placeholder2, LV_OBJ_FLAG_CLICKABLE);

#if APP_DEMO_MUSIC_SQUARE
    lv_obj_t * placeholder3 = lv_obj_create(main_cont);
    lv_obj_remove_style_all(placeholder3);
    lv_obj_remove_flag(placeholder3, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_size(placeholder1, lv_pct(100), LV_VER_RES);
    lv_obj_set_y(placeholder1, 0);

    lv_obj_set_size(placeholder2, lv_pct(100), LV_VER_RES);
    lv_obj_set_y(placeholder2, LV_VER_RES);

    lv_obj_set_size(placeholder3, lv_pct(100),  LV_VER_RES - 2 * APP_DEMO_MUSIC_HANDLE_SIZE);
    lv_obj_set_y(placeholder3, 2 * LV_VER_RES + APP_DEMO_MUSIC_HANDLE_SIZE);
#else
    lv_obj_set_size(placeholder1, lv_pct(100), LV_VER_RES);
    lv_obj_set_y(placeholder1, 0);

    lv_obj_set_size(placeholder2, lv_pct(100),  LV_VER_RES - 2 * APP_DEMO_MUSIC_HANDLE_SIZE);
    lv_obj_set_y(placeholder2, LV_VER_RES + APP_DEMO_MUSIC_HANDLE_SIZE);
#endif

    lv_obj_update_layout(main_cont);

    return player;
}

static void create_wave_images(lv_obj_t * parent)
{
    LV_IMAGE_DECLARE(img_lv_demo_music_wave_top);
    LV_IMAGE_DECLARE(img_lv_demo_music_wave_bottom);
    lv_obj_t * wave_top = lv_image_create(parent);
    lv_image_set_src(wave_top, &img_lv_demo_music_wave_top);
    lv_image_set_inner_align(wave_top, LV_IMAGE_ALIGN_TILE);
    lv_obj_set_width(wave_top, LV_HOR_RES);
    lv_obj_align(wave_top, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_add_flag(wave_top, LV_OBJ_FLAG_IGNORE_LAYOUT);

    lv_obj_t * wave_bottom = lv_image_create(parent);
    lv_image_set_src(wave_bottom, &img_lv_demo_music_wave_bottom);
    lv_image_set_inner_align(wave_bottom, LV_IMAGE_ALIGN_TILE);
    lv_obj_set_width(wave_bottom, LV_HOR_RES);
    lv_obj_align(wave_bottom, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(wave_bottom, LV_OBJ_FLAG_IGNORE_LAYOUT);

    LV_IMAGE_DECLARE(img_lv_demo_music_corner_left);
    LV_IMAGE_DECLARE(img_lv_demo_music_corner_right);
    lv_obj_t * wave_corner = lv_image_create(parent);
    lv_image_set_src(wave_corner, &img_lv_demo_music_corner_left);
#if APP_DEMO_MUSIC_ROUND == 0
    lv_obj_align(wave_corner, LV_ALIGN_BOTTOM_LEFT, 0, 0);
#else
    lv_obj_align(wave_corner, LV_ALIGN_BOTTOM_LEFT, -LV_HOR_RES / 6, 0);
#endif
    lv_obj_add_flag(wave_corner, LV_OBJ_FLAG_IGNORE_LAYOUT);

    wave_corner = lv_image_create(parent);
    lv_image_set_src(wave_corner, &img_lv_demo_music_corner_right);
#if APP_DEMO_MUSIC_ROUND == 0
    lv_obj_align(wave_corner, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
#else
    lv_obj_align(wave_corner, LV_ALIGN_BOTTOM_RIGHT, LV_HOR_RES / 6, 0);
#endif
    lv_obj_add_flag(wave_corner, LV_OBJ_FLAG_IGNORE_LAYOUT);
}

static lv_obj_t * create_title_box(lv_obj_t * parent)
{

    /*Create the titles*/
    bool has_tracks = lv_demo_music_get_track_count() > 0;
    lv_obj_t * cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_height(cont, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    title_label = lv_label_create(cont);
    lv_obj_set_style_text_font(title_label, font_large, 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0x504d6d), 0);
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(title_label, lv_pct(100));
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(title_label, has_tracks ? lv_demo_music_get_title(track_id) : "No tracks found");
    lv_obj_set_height(title_label, lv_font_get_line_height(font_large) * 3 / 2);

    artist_label = lv_label_create(cont);
    lv_obj_set_style_text_font(artist_label, font_small, 0);
    lv_obj_set_style_text_color(artist_label, lv_color_hex(0x504d6d), 0);
    lv_obj_set_style_text_align(artist_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(artist_label, lv_pct(100));
    lv_label_set_long_mode(artist_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(artist_label, has_tracks ? lv_demo_music_get_artist(track_id) : "");

    genre_label = lv_label_create(cont);
    lv_obj_set_style_text_font(genre_label, font_small, 0);
    lv_obj_set_style_text_color(genre_label, lv_color_hex(0x8a86b8), 0);
    lv_obj_set_style_text_align(genre_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(genre_label, lv_pct(100));
    lv_label_set_long_mode(genre_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(genre_label, has_tracks ? lv_demo_music_get_genre(track_id) : "");

    return cont;
}

static lv_obj_t * create_icon_box(lv_obj_t * parent)
{

    lv_obj_t * cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_height(cont, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * icon;
    LV_IMAGE_DECLARE(img_lv_demo_music_icon_1);
    LV_IMAGE_DECLARE(img_lv_demo_music_icon_2);
    LV_IMAGE_DECLARE(img_lv_demo_music_icon_3);
    LV_IMAGE_DECLARE(img_lv_demo_music_icon_4);
    icon = lv_image_create(cont);
    lv_image_set_src(icon, &img_lv_demo_music_icon_1);
    icon = lv_image_create(cont);
    lv_image_set_src(icon, &img_lv_demo_music_icon_2);
    icon = lv_image_create(cont);
    lv_image_set_src(icon, &img_lv_demo_music_icon_3);
    icon = lv_image_create(cont);
    lv_image_set_src(icon, &img_lv_demo_music_icon_4);

    return cont;
}

static lv_obj_t * create_spectrum_obj(lv_obj_t * parent)
{
    /*Create the spectrum visualizer*/
    lv_obj_t * obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
#if APP_DEMO_MUSIC_LARGE
    lv_obj_set_height(obj, 500);
#else
    lv_obj_set_height(obj, MUSIC_SPECTRUM_HEIGHT);
#endif
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(obj, spectrum_draw_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_refresh_ext_draw_size(obj);
    album_image_obj = album_image_create(obj);
    return obj;
}

static lv_obj_t * create_ctrl_box(lv_obj_t * parent)
{
    /*Create the control box*/
    lv_obj_t * cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_height(cont, LV_SIZE_CONTENT);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLL_ON_FOCUS | LV_OBJ_FLAG_SCROLLABLE);
#if APP_DEMO_MUSIC_LARGE
    lv_obj_set_style_pad_bottom(cont, 17, 0);
#else
    lv_obj_set_style_pad_bottom(cont, 8, 0);
#endif
#if APP_DEMO_MUSIC_ROUND
    /* Native assets are 24 / 62 / 79 / 62 / 24 px wide.  Explicit columns
     * prevent the 79 px play button from overlapping its neighbours in the
     * 326 px round safe area. */
    static const int32_t grid_col[] = {
        LV_GRID_FR(1), 24, 8, 62, 4, 79, 4, 62, 8, 24, LV_GRID_FR(1),
        LV_GRID_TEMPLATE_LAST
    };
#elif MUSIC_COMPACT_TOUCH_CONTROLS
    /* Keep the enlarged play hit area clear of the previous/next buttons. */
    static const int32_t grid_col[] = {LV_GRID_FR(2), LV_GRID_FR(3), LV_GRID_FR(5), 116, LV_GRID_FR(5), LV_GRID_FR(3), LV_GRID_FR(2), LV_GRID_TEMPLATE_LAST};
#else
    static const int32_t grid_col[] = {LV_GRID_FR(2), LV_GRID_FR(3), LV_GRID_FR(5), LV_GRID_FR(5), LV_GRID_FR(5), LV_GRID_FR(3), LV_GRID_FR(2), LV_GRID_TEMPLATE_LAST};
#endif
#if MUSIC_COMPACT_TOUCH_CONTROLS
    /* The 112 px row includes the scaled touch area; leave room for the slider knob. */
    static const int32_t grid_row[] = {112, LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST};
    lv_obj_set_style_pad_row(cont, 16, 0);
#else
    static const int32_t grid_row[] = {LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST};
#endif
    lv_obj_set_grid_dsc_array(cont, grid_col, grid_row);

    LV_IMAGE_DECLARE(img_lv_demo_music_btn_loop);
    LV_IMAGE_DECLARE(img_lv_demo_music_btn_rnd);
    LV_IMAGE_DECLARE(img_lv_demo_music_btn_next);
    LV_IMAGE_DECLARE(img_lv_demo_music_btn_prev);
    LV_IMAGE_DECLARE(img_lv_demo_music_btn_play);
    LV_IMAGE_DECLARE(img_lv_demo_music_btn_pause);

    lv_obj_t * icon;
    icon = lv_image_create(cont);
    lv_image_set_src(icon, &img_lv_demo_music_btn_rnd);
#if APP_DEMO_MUSIC_ROUND
    lv_obj_set_grid_cell(icon, LV_GRID_ALIGN_CENTER, 1, 1, LV_GRID_ALIGN_CENTER, 0, 1);
#else
    lv_obj_set_grid_cell(icon, LV_GRID_ALIGN_START, 1, 1, LV_GRID_ALIGN_CENTER, 0, 1);
#endif

    icon = lv_image_create(cont);
    lv_image_set_src(icon, &img_lv_demo_music_btn_loop);
#if APP_DEMO_MUSIC_ROUND
    lv_obj_set_grid_cell(icon, LV_GRID_ALIGN_CENTER, 9, 1, LV_GRID_ALIGN_CENTER, 0, 1);
#else
    lv_obj_set_grid_cell(icon, LV_GRID_ALIGN_END, 5, 1, LV_GRID_ALIGN_CENTER, 0, 1);
#endif

    lv_obj_t * prev_obj = lv_image_create(cont);
    lv_image_set_src(prev_obj, &img_lv_demo_music_btn_prev);
#if APP_DEMO_MUSIC_ROUND
    lv_obj_set_grid_cell(prev_obj, LV_GRID_ALIGN_CENTER, 3, 1, LV_GRID_ALIGN_CENTER, 0, 1);
#else
    lv_obj_set_grid_cell(prev_obj, LV_GRID_ALIGN_CENTER, 2, 1, LV_GRID_ALIGN_CENTER, 0, 1);
#endif
    lv_obj_add_event_cb(prev_obj, prev_click_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(prev_obj, LV_OBJ_FLAG_CLICKABLE);

    play_obj = lv_imagebutton_create(cont);
    lv_imagebutton_set_src(play_obj, LV_IMAGEBUTTON_STATE_RELEASED, NULL, &img_lv_demo_music_btn_play, NULL);
    lv_imagebutton_set_src(play_obj, LV_IMAGEBUTTON_STATE_CHECKED_RELEASED, NULL, &img_lv_demo_music_btn_pause, NULL);
    /* Playback commands own CHECKED; automatic release toggling can undo a press action. */
#if APP_DEMO_MUSIC_ROUND
    lv_obj_set_grid_cell(play_obj, LV_GRID_ALIGN_CENTER, 5, 1, LV_GRID_ALIGN_CENTER, 0, 1);
#else
    lv_obj_set_grid_cell(play_obj, LV_GRID_ALIGN_CENTER, 3, 1, LV_GRID_ALIGN_CENTER, 0, 1);
#endif

    lv_obj_add_event_cb(play_obj, play_event_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_flag(play_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(play_obj, img_lv_demo_music_btn_play.header.w);
#if MUSIC_COMPACT_TOUCH_CONTROLS
    /* Scale the whole image button so its drawing and hit coordinates agree. */
    lv_obj_set_style_transform_pivot_x(play_obj, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(play_obj, lv_pct(50), 0);
    lv_obj_set_style_transform_scale_x(play_obj, 307, 0);
    lv_obj_set_style_transform_scale_y(play_obj, 307, 0);
    lv_obj_set_ext_click_area(play_obj, 8);
    lv_obj_add_flag(play_obj, LV_OBJ_FLAG_PRESS_LOCK);
    /* A slightly moving tap must not start scrolling the player and cancel the click. */
    lv_obj_remove_flag(play_obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER);
#endif

    lv_obj_t * next_obj = lv_image_create(cont);
    lv_image_set_src(next_obj, &img_lv_demo_music_btn_next);
#if APP_DEMO_MUSIC_ROUND
    lv_obj_set_grid_cell(next_obj, LV_GRID_ALIGN_CENTER, 7, 1, LV_GRID_ALIGN_CENTER, 0, 1);
#else
    lv_obj_set_grid_cell(next_obj, LV_GRID_ALIGN_CENTER, 4, 1, LV_GRID_ALIGN_CENTER, 0, 1);
#endif
    lv_obj_add_event_cb(next_obj, next_click_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(next_obj, LV_OBJ_FLAG_CLICKABLE);

    if(lv_demo_music_get_track_count() == 0) {
        lv_obj_t * controls[] = {prev_obj, play_obj, next_obj};
        for(size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) {
            lv_obj_add_state(controls[i], LV_STATE_DISABLED);
            lv_obj_remove_flag(controls[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_opa(controls[i], LV_OPA_40, 0);
        }
    }

    LV_IMAGE_DECLARE(img_lv_demo_music_slider_knob);
    slider_obj = lv_slider_create(cont);
    lv_obj_set_style_anim_duration(slider_obj, 100, 0);
    lv_obj_add_flag(slider_obj, LV_OBJ_FLAG_CLICKABLE); /*No input from the slider*/
    lv_obj_remove_flag(slider_obj, LV_OBJ_FLAG_SCROLL_ON_FOCUS);

#if APP_DEMO_MUSIC_LARGE == 0
    lv_obj_set_height(slider_obj, 3);
#else
    lv_obj_set_height(slider_obj, 6);
#endif
#if APP_DEMO_MUSIC_ROUND
    /* Leave room for the 36 px knob at 100% and for the adjacent time label. */
    lv_obj_set_width(slider_obj, APP_DEMO_MUSIC_ROUND_PROGRESS_WIDTH);
    lv_obj_set_grid_cell(slider_obj, LV_GRID_ALIGN_START, 1, 7, LV_GRID_ALIGN_CENTER, 1, 1);
#else
    lv_obj_set_grid_cell(slider_obj, LV_GRID_ALIGN_STRETCH, 1, 4, LV_GRID_ALIGN_CENTER, 1, 1);
#endif

    lv_obj_set_style_bg_image_src(slider_obj, &img_lv_demo_music_slider_knob, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider_obj, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider_obj, 20, LV_PART_KNOB);
    lv_obj_set_style_bg_grad_dir(slider_obj, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_obj, lv_color_hex(0x569af8), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(slider_obj, lv_color_hex(0xa666f1), LV_PART_INDICATOR);
    lv_obj_set_style_outline_width(slider_obj, 0, 0);
    lv_obj_add_event_cb(slider_obj, del_counter_timer_cb, LV_EVENT_DELETE, NULL);

    time_obj = lv_label_create(cont);
    lv_obj_set_style_text_font(time_obj, font_small, 0);
    lv_obj_set_style_text_color(time_obj, lv_color_hex(0x8a86b8), 0);
    lv_label_set_text(time_obj, "0:00");
#if APP_DEMO_MUSIC_ROUND
    lv_obj_set_grid_cell(time_obj, LV_GRID_ALIGN_CENTER, 9, 1, LV_GRID_ALIGN_CENTER, 1, 1);
#else
    lv_obj_set_grid_cell(time_obj, LV_GRID_ALIGN_END, 5, 1, LV_GRID_ALIGN_CENTER, 1, 1);
#endif
    lv_obj_add_event_cb(time_obj, del_counter_timer_cb, LV_EVENT_DELETE, NULL);

    return cont;
}

static lv_obj_t * create_handle(lv_obj_t * parent)
{
    lv_obj_t * cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);

    lv_obj_set_size(cont, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cont, 8, 0);

    /*A handle to scroll to the track list*/
    lv_obj_t * handle_label = lv_label_create(cont);
    lv_label_set_text(handle_label, "ALL TRACKS");
    lv_obj_set_style_text_font(handle_label, font_small, 0);
    lv_obj_set_style_text_color(handle_label, lv_color_hex(0x8a86b8), 0);

    lv_obj_t * handle_rect = lv_obj_create(cont);
#if APP_DEMO_MUSIC_LARGE
    lv_obj_set_size(handle_rect, 40, 3);
#else
    lv_obj_set_size(handle_rect, 20, 2);
#endif

    lv_obj_set_style_bg_color(handle_rect, lv_color_hex(0x8a86b8), 0);
    lv_obj_set_style_border_width(handle_rect, 0, 0);

    return cont;
}

static void track_load(uint32_t id)
{
    uint32_t track_count = lv_demo_music_get_track_count();
    if(id >= track_count) {
        return;
    }

    spectrum_i = 0;
    time_act = 0;
    spectrum_i_pause = 0;
    lv_slider_set_value(slider_obj, 0, LV_ANIM_OFF);
    lv_label_set_text(time_obj, "0:00");

    if(id == track_id) return;
    bool next = false;
    if((track_id + 1) % track_count == id) next = true;

    lv_demo_music_list_button_check(track_id, false);

    track_id = id;

    lv_demo_music_list_button_check(id, true);

    lv_label_set_text(title_label, lv_demo_music_get_title(track_id));
    lv_label_set_text(artist_label, lv_demo_music_get_artist(track_id));
    lv_label_set_text(genre_label, lv_demo_music_get_genre(track_id));

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, album_image_obj);
    lv_anim_set_values(&a, lv_obj_get_style_image_opa(album_image_obj, 0), LV_OPA_TRANSP);
    lv_anim_set_exec_cb(&a, album_fade_anim_cb);
    lv_anim_set_duration(&a, 500);
    lv_anim_start(&a);

    lv_anim_init(&a);
    lv_anim_set_var(&a, album_image_obj);
    lv_anim_set_duration(&a, 500);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
#if APP_DEMO_MUSIC_LANDSCAPE
    if(next) {
        lv_anim_set_values(&a, 0, - LV_HOR_RES / 7);
    }
    else {
        lv_anim_set_values(&a, 0, LV_HOR_RES / 7);
    }
#else
    if(next) {
        lv_anim_set_values(&a, 0, - LV_HOR_RES / 2);
    }
    else {
        lv_anim_set_values(&a, 0, LV_HOR_RES / 2);
    }
#endif
    lv_anim_set_exec_cb(&a, _obj_set_x_anim_cb);
    lv_anim_set_completed_cb(&a, lv_obj_delete_anim_completed_cb);
    lv_anim_start(&a);

    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_var(&a, album_image_obj);
    lv_anim_set_duration(&a, 500);
    lv_anim_set_values(&a, LV_SCALE_NONE, LV_SCALE_NONE / 2);
    lv_anim_set_exec_cb(&a, _image_set_scale_anim_cb);
    lv_anim_set_completed_cb(&a, NULL);
    lv_anim_start(&a);

    album_image_obj = album_image_create(spectrum_obj);

    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_set_var(&a, album_image_obj);
    lv_anim_set_duration(&a, 500);
    lv_anim_set_delay(&a, 100);
    lv_anim_set_values(&a, LV_SCALE_NONE / 4, LV_SCALE_NONE);
    lv_anim_set_exec_cb(&a, _image_set_scale_anim_cb);
    lv_anim_set_completed_cb(&a, NULL);
    lv_anim_start(&a);

    lv_anim_init(&a);
    lv_anim_set_var(&a, album_image_obj);
    lv_anim_set_values(&a, 0, LV_OPA_COVER);
    lv_anim_set_exec_cb(&a, album_fade_anim_cb);
    lv_anim_set_duration(&a, 500);
    lv_anim_set_delay(&a, 100);
    lv_anim_start(&a);
}

int32_t get_cos(int32_t deg, int32_t a)
{
    int32_t r = (lv_trigo_cos(deg) * a);

    r += LV_TRIGO_SIN_MAX / 2;
    return r >> LV_TRIGO_SHIFT;
}

int32_t get_sin(int32_t deg, int32_t a)
{
    int32_t r = lv_trigo_sin(deg) * a;

    return (r + LV_TRIGO_SIN_MAX / 2) >> LV_TRIGO_SHIFT;

}

static void del_counter_timer_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_DELETE && sec_counter_timer) {
        lv_timer_delete(sec_counter_timer);
        sec_counter_timer = NULL;
    }
}

static void spectrum_draw_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if(code == LV_EVENT_REFR_EXT_DRAW_SIZE) {
#if MUSIC_COMPACT_TOUCH_CONTROLS
        /* Compact bars stay inside the spectrum object, so animation needn't dirty the whole panel. */
        lv_event_set_ext_draw_size(e, 0);
#elif APP_DEMO_MUSIC_LANDSCAPE
        lv_event_set_ext_draw_size(e, LV_HOR_RES);
#else
        lv_event_set_ext_draw_size(e, LV_VER_RES);
#endif
    }
    else if(code == LV_EVENT_COVER_CHECK) {
        lv_event_set_cover_res(e, LV_COVER_RES_NOT_COVER);
    }
    else if(code == LV_EVENT_DRAW_MAIN_BEGIN) {
        lv_obj_t * obj = lv_event_get_target(e);
        lv_layer_t * layer = lv_event_get_layer(e);

        lv_opa_t opa = lv_obj_get_style_opa_recursive(obj, LV_PART_MAIN);
        if(opa < LV_OPA_MIN) return;

        lv_point_t center;
        lv_area_t obj_coords;
        lv_obj_get_coords(obj, &obj_coords);
        center.x = obj_coords.x1 + lv_obj_get_width(obj) / 2;
        center.y = obj_coords.y1 + lv_obj_get_height(obj) / 2;

        lv_draw_triangle_dsc_t draw_dsc;
        lv_draw_triangle_dsc_init(&draw_dsc);
        draw_dsc.opa = LV_OPA_COVER;

        uint16_t r[64];
        uint32_t i;

        for(i = 0; i < BAR_CNT; i++) {
            r[i] = BAR_REST_RADIUS;
        }

        uint32_t s;
        for(s = 0; s < 4; s++) {
            uint32_t f;
            uint32_t band_w = 0;    /*Real number of bars in this band.*/
            switch(s) {
                case 0:
                    band_w = 20;
                    break;
                case 1:
                    band_w = 8;
                    break;
                case 2:
                    band_w = 4;
                    break;
                case 3:
                    band_w = 2;
                    break;
            }

            /* Add "side bars" with cosine characteristic.*/
            for(f = 0; f < band_w; f++) {
                uint32_t ampl_main = spectrum[spectrum_i][s];
                int32_t ampl_mod = get_cos(f * 360 / band_w + 180, 180) + 180;
                int32_t t = BAR_PER_BAND_CNT * s - band_w / 2 + f;
                if(t < 0) t = BAR_CNT + t;
                if(t >= BAR_CNT) t = t - BAR_CNT;
                r[t] += (ampl_main * ampl_mod) >> 9;
            }
        }

        const int32_t amax = 20;
        int32_t animv = LV_CLAMP(0, (int32_t)spectrum_i - (int32_t)spectrum_lane_ofs_start, amax);
#if MUSIC_COMPACT_TOUCH_CONTROLS
        const uint32_t max_bar_radius = LV_MIN(lv_obj_get_width(obj), lv_obj_get_height(obj)) / 2 - 2;
#endif
        for(i = 0; i < BAR_CNT; i++) {
            uint32_t deg_space = 1;
            uint32_t deg = i * DEG_STEP + 90;
            uint32_t j = (i + bar_rot + rnd_array[bar_ofs % 10]) % BAR_CNT;
            uint32_t k = (i + bar_rot + rnd_array[(bar_ofs + 1) % 10]) % BAR_CNT;

            uint32_t v;
            if(start_anim) {
                v = BAR_REST_RADIUS + start_anim_values[i];
            }
            else {
                v = (r[k] * animv + r[j] * (amax - animv)) / amax;
            }
#if MUSIC_COMPACT_TOUCH_CONTROLS
            v = LV_MIN(v, max_bar_radius);
#endif

            if(v < BAR_COLOR1_STOP) draw_dsc.color = BAR_COLOR1;
            else if(v > (uint32_t)BAR_COLOR3_STOP) draw_dsc.color = BAR_COLOR3;
            else if(v > BAR_COLOR2_STOP) draw_dsc.color = lv_color_mix(BAR_COLOR3, BAR_COLOR2,
                                                                              ((v - BAR_COLOR2_STOP) * 255) / (BAR_COLOR3_STOP - BAR_COLOR2_STOP));
            else draw_dsc.color = lv_color_mix(BAR_COLOR2, BAR_COLOR1,
                                                      ((v - BAR_COLOR1_STOP) * 255) / (BAR_COLOR2_STOP - BAR_COLOR1_STOP));

            uint32_t di = deg + deg_space;

            int32_t x1_out = get_cos(di, v);
            draw_dsc.p[0].x = center.x + x1_out;
            draw_dsc.p[0].y = center.y + get_sin(di, v);

            di += DEG_STEP - deg_space * 2;

            int32_t x2_out = get_cos(di, v);
            draw_dsc.p[1].x = center.x + x2_out;
            draw_dsc.p[1].y = center.y + get_sin(di, v);

            int32_t x2_in = get_cos(di, 0);
            draw_dsc.p[2].x = center.x + x2_in;
            draw_dsc.p[2].y = center.y + get_sin(di, 0);
            lv_draw_triangle(layer, &draw_dsc);

            draw_dsc.p[0].x = center.x - x1_out;
            draw_dsc.p[1].x = center.x - x2_out;
            draw_dsc.p[2].x = center.x - x2_in;
            lv_draw_triangle(layer, &draw_dsc);
        }
    }
    else if(code == LV_EVENT_DELETE) {
        lv_anim_delete(NULL, start_anim_cb);
        lv_anim_delete(NULL, spectrum_anim_cb);
        if(start_anim && stop_start_anim_timer) {
            lv_timer_delete(stop_start_anim_timer);
            stop_start_anim_timer = NULL;
        }
    }
}

static bool spectrum_refresh_ready(void)
{
#if MUSIC_COMPACT_TOUCH_CONTROLS
    /* LVGL runs animations every 15 ms; leave time for input between spectrum frames. */
    if(lv_tick_elaps(last_spectrum_draw_tick) < MUSIC_SPECTRUM_REFRESH_PERIOD_MS) {
        return false;
    }
    last_spectrum_draw_tick = lv_tick_get();
#endif
    return true;
}

static void spectrum_anim_cb(void * a, int32_t v)
{
    lv_obj_t * obj = a;
    if(!spectrum_refresh_ready()) return;
    if(start_anim) {
        lv_obj_invalidate(obj);
        return;
    }

    spectrum_i = v;
    lv_obj_invalidate(obj);

    static uint32_t bass_cnt = 0;
    static int32_t last_bass = -1000;
    static int32_t dir = 1;
    if(spectrum[spectrum_i][0] > 12) {
        if(spectrum_i - last_bass > 5) {
            bass_cnt++;
            last_bass = spectrum_i;
            if(bass_cnt >= 2) {
                bass_cnt = 0;
                spectrum_lane_ofs_start = spectrum_i;
                bar_ofs++;
            }
        }
    }
    if(spectrum[spectrum_i][0] < 4) bar_rot += dir;

    lv_image_set_scale(album_image_obj, LV_SCALE_NONE + spectrum[spectrum_i][0]);
}

static void start_anim_cb(void * var, int32_t v)
{
    int32_t * av = var;
    *av = v;
    if(spectrum_refresh_ready()) lv_obj_invalidate(spectrum_obj);
}

static lv_obj_t * album_image_create(lv_obj_t * parent)
{
    LV_IMAGE_DECLARE(img_lv_demo_music_cover_1);
    LV_IMAGE_DECLARE(img_lv_demo_music_cover_2);
    LV_IMAGE_DECLARE(img_lv_demo_music_cover_3);

    lv_obj_t * img;
    img = lv_image_create(parent);

    /* Reuse the three bundled covers and spectra for every library track. */
    switch(track_id % 3) {
        case 2:
            lv_image_set_src(img, &img_lv_demo_music_cover_3);
            spectrum = spectrum_3;
            spectrum_len = sizeof(spectrum_3) / sizeof(spectrum_3[0]);
            break;
        case 1:
            lv_image_set_src(img, &img_lv_demo_music_cover_2);
            spectrum = spectrum_2;
            spectrum_len = sizeof(spectrum_2) / sizeof(spectrum_2[0]);
            break;
        case 0:
            lv_image_set_src(img, &img_lv_demo_music_cover_1);
            spectrum = spectrum_1;
            spectrum_len = sizeof(spectrum_1) / sizeof(spectrum_1[0]);
            break;
    }
    lv_image_set_antialias(img, false);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(img, album_gesture_event_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);

    return img;

}

static void album_gesture_event_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if(dir == LV_DIR_LEFT) lv_demo_music_album_next(true);
    if(dir == LV_DIR_RIGHT) lv_demo_music_album_next(false);
}

static void play_event_press_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    if(playing) {
        lv_demo_music_pause();
    }
    else {
        lv_demo_music_resume();
    }
}

static void prev_click_event_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    lv_demo_music_album_next(false);
}

static void next_click_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        lv_demo_music_album_next(true);
    }
}

static void timer_cb(lv_timer_t * t)
{
    LV_UNUSED(t);
    time_act++;
    lv_label_set_text_fmt(time_obj, "%"LV_PRIu32":%02"LV_PRIu32, time_act / 60, time_act % 60);
    lv_slider_set_value(slider_obj, time_act, LV_ANIM_ON);
}

static void spectrum_end_cb(lv_anim_t * a)
{
    LV_UNUSED(a);
    lv_demo_music_album_next(true);
}

static void stop_start_anim(lv_timer_t * t)
{
    LV_UNUSED(t);
    start_anim = false;
    stop_start_anim_timer = NULL;
    lv_obj_refresh_ext_draw_size(spectrum_obj);
}

static void album_fade_anim_cb(void * var, int32_t v)
{
    lv_obj_set_style_image_opa(var, v, 0);
}
#endif /*LV_USE_DEMO_MUSIC*/
