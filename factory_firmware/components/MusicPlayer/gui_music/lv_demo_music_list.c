/**
 * @file lv_demo_music_list.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "lv_demo_music_list.h"
#if APP_DEMO_MUSIC_ENABLE

#include "lv_demo_music_main.h"

#include "lv_demo_music.h"
/*********************
 *      DEFINES
 *********************/
#define MUSIC_LIST_PAGE_SIZE 5U

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static lv_obj_t * add_list_button(lv_obj_t * parent, uint32_t track_id);
static void add_page_footer(lv_obj_t * parent, uint32_t page_count);
static void render_page(uint32_t page);
static void page_change_async_cb(void * user_data);
static void page_click_event_cb(lv_event_t * e);
static void btn_click_event_cb(lv_event_t * e);
static void list_delete_event_cb(lv_event_t * e);

/**********************
 *  STATIC VARIABLES
 **********************/
static lv_obj_t * list;
static const lv_font_t * font_small;
static const lv_font_t * font_medium;
static lv_style_t style_scrollbar;
static lv_style_t style_btn;
static lv_style_t style_button_pr;
static lv_style_t style_button_chk;
static lv_style_t style_button_dis;
static lv_style_t style_title;
static lv_style_t style_artist;
static lv_style_t style_time;
static uint32_t page_index;
static uint32_t page_first_track;
static uint32_t page_track_count;
static uint32_t selected_track_id;
static uint8_t page_previous_event;
static uint8_t page_next_event;
LV_IMAGE_DECLARE(img_lv_demo_music_btn_list_play);
LV_IMAGE_DECLARE(img_lv_demo_music_btn_list_pause);

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_obj_t * lv_demo_music_list_create(lv_obj_t * parent)
{
#if APP_DEMO_MUSIC_LARGE
    font_small = &lv_font_montserrat_16;
    font_medium = &lv_font_montserrat_22;
#else
    font_small = &lv_font_montserrat_12;
    font_medium = &lv_font_montserrat_16;
#endif

    lv_style_init(&style_scrollbar);
    lv_style_set_width(&style_scrollbar,  4);
    lv_style_set_bg_opa(&style_scrollbar, LV_OPA_COVER);
    lv_style_set_bg_color(&style_scrollbar, lv_color_hex3(0xeee));
    lv_style_set_radius(&style_scrollbar, LV_RADIUS_CIRCLE);
    lv_style_set_pad_right(&style_scrollbar, 4);

    static const int32_t grid_cols[] = {LV_GRID_CONTENT, LV_GRID_FR(1), LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST};
#if APP_DEMO_MUSIC_LARGE
    static const int32_t grid_rows[] = {35,  30, LV_GRID_TEMPLATE_LAST};
#else
    static const int32_t grid_rows[] = {22,  17, LV_GRID_TEMPLATE_LAST};
#endif
    lv_style_init(&style_btn);
    lv_style_set_bg_opa(&style_btn, LV_OPA_TRANSP);
    lv_style_set_grid_column_dsc_array(&style_btn, grid_cols);
    lv_style_set_grid_row_dsc_array(&style_btn, grid_rows);
    lv_style_set_grid_row_align(&style_btn, LV_GRID_ALIGN_CENTER);
    lv_style_set_layout(&style_btn, LV_LAYOUT_GRID);
#if APP_DEMO_MUSIC_LARGE
    lv_style_set_pad_right(&style_btn, 30);
#else
    lv_style_set_pad_right(&style_btn, 20);
#endif
    lv_style_init(&style_button_pr);
    lv_style_set_bg_opa(&style_button_pr, LV_OPA_COVER);
    lv_style_set_bg_color(&style_button_pr,  lv_color_hex(0x4c4965));

    lv_style_init(&style_button_chk);
    lv_style_set_bg_opa(&style_button_chk, LV_OPA_COVER);
    lv_style_set_bg_color(&style_button_chk, lv_color_hex(0x4c4965));

    lv_style_init(&style_button_dis);
    lv_style_set_text_opa(&style_button_dis, LV_OPA_40);
    lv_style_set_image_opa(&style_button_dis, LV_OPA_40);

    lv_style_init(&style_title);
    lv_style_set_text_font(&style_title, font_medium);
    lv_style_set_text_color(&style_title, lv_color_hex(0xffffff));

    lv_style_init(&style_artist);
    lv_style_set_text_font(&style_artist, font_small);
    lv_style_set_text_color(&style_artist, lv_color_hex(0xb1b0be));

    lv_style_init(&style_time);
    lv_style_set_text_font(&style_time, font_medium);
    lv_style_set_text_color(&style_time, lv_color_hex(0xffffff));

    /*Create an empty transparent container*/
    list = lv_obj_create(parent);
    lv_obj_add_event_cb(list, list_delete_event_cb, LV_EVENT_DELETE, NULL);
    lv_obj_remove_style_all(list);
#if APP_DEMO_MUSIC_ROUND
    /* Exact 326 x 326 inscribed square: x/y = 70..395 on the 466 px circle. */
    lv_obj_set_size(list, APP_DEMO_MUSIC_ROUND_SAFE_SIZE, APP_DEMO_MUSIC_ROUND_SAFE_SIZE);
    lv_obj_center(list);
#else
    lv_obj_set_size(list, LV_HOR_RES, LV_VER_RES - APP_DEMO_MUSIC_HANDLE_SIZE);
    lv_obj_set_y(list, APP_DEMO_MUSIC_HANDLE_SIZE);
#endif
    lv_obj_add_style(list, &style_scrollbar, LV_PART_SCROLLBAR);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    page_index = 0;
    page_first_track = 0;
    page_track_count = 0;
    selected_track_id = 0;
    render_page(0);

#if APP_DEMO_MUSIC_ROUND
    lv_obj_set_scroll_snap_y(list, LV_SCROLL_SNAP_CENTER);
#endif

    lv_demo_music_list_button_check(0, true);

    return list;
}

void lv_demo_music_list_button_check(uint32_t track_id, bool state)
{
    if(list == NULL || track_id >= lv_demo_music_get_track_count()) {
        return;
    }

    if(state) {
        selected_track_id = track_id;
        const uint32_t requested_page = track_id / MUSIC_LIST_PAGE_SIZE;
        if(requested_page != page_index) {
            render_page(requested_page);
        }
    }

    if(track_id < page_first_track ||
       track_id >= page_first_track + page_track_count) {
        return;
    }

    lv_obj_t * btn = lv_obj_get_child(list, track_id - page_first_track);
    if(btn == NULL) {
        return;
    }

    lv_obj_t * icon = lv_obj_get_child(btn, 0);

    if(state) {
        lv_obj_add_state(btn, LV_STATE_CHECKED);
        lv_image_set_src(icon, &img_lv_demo_music_btn_list_pause);
        lv_obj_scroll_to_view(btn, LV_ANIM_ON);
    }
    else {
        lv_obj_remove_state(btn, LV_STATE_CHECKED);
        lv_image_set_src(icon, &img_lv_demo_music_btn_list_play);
    }
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static lv_obj_t * add_list_button(lv_obj_t * parent, uint32_t track_id)
{
    uint32_t t = lv_demo_music_get_track_length(track_id);
    char time[32];
    lv_snprintf(time, sizeof(time), "%"LV_PRIu32":%02"LV_PRIu32, t / 60, t % 60);
    const char * title = lv_demo_music_get_title(track_id);
    const char * artist = lv_demo_music_get_artist(track_id);

    lv_obj_t * btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
#if APP_DEMO_MUSIC_LARGE
    lv_obj_set_size(btn, lv_pct(100), 110);
#else
    lv_obj_set_size(btn, lv_pct(100), 60);
#endif

    lv_obj_add_style(btn, &style_btn, 0);
    lv_obj_add_style(btn, &style_button_pr, LV_STATE_PRESSED);
    lv_obj_add_style(btn, &style_button_chk, LV_STATE_CHECKED);
    lv_obj_add_style(btn, &style_button_dis, LV_STATE_DISABLED);
    lv_obj_add_event_cb(btn, btn_click_event_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)track_id);

    lv_obj_t * icon = lv_image_create(btn);
    lv_image_set_src(icon, &img_lv_demo_music_btn_list_play);
    lv_obj_set_grid_cell(icon, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, 0, 2);

    lv_obj_t * title_label = lv_label_create(btn);
    lv_label_set_text(title_label, title);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_grid_cell(title_label, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_CENTER, 0, 1);
    lv_obj_add_style(title_label, &style_title, 0);

    lv_obj_t * artist_label = lv_label_create(btn);
    lv_label_set_text(artist_label, artist);
    lv_label_set_long_mode(artist_label, LV_LABEL_LONG_DOT);
    lv_obj_add_style(artist_label, &style_artist, 0);
    lv_obj_set_grid_cell(artist_label, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_CENTER, 1, 1);

    lv_obj_t * time_label = lv_label_create(btn);
    lv_label_set_text(time_label, time);
    lv_obj_add_style(time_label, &style_time, 0);
    lv_obj_set_grid_cell(time_label, LV_GRID_ALIGN_END, 2, 1, LV_GRID_ALIGN_CENTER, 0, 2);

    LV_IMAGE_DECLARE(img_lv_demo_music_list_border);
    lv_obj_t * border = lv_image_create(btn);
    lv_image_set_src(border, &img_lv_demo_music_list_border);
    lv_image_set_inner_align(border, LV_IMAGE_ALIGN_TILE);
#if APP_DEMO_MUSIC_ROUND
    lv_obj_set_width(border, lv_pct(100));
#else
    lv_obj_set_width(border, lv_pct(120));
#endif
    lv_obj_align(border, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(border, LV_OBJ_FLAG_IGNORE_LAYOUT);

    return btn;
}

static void add_page_footer(lv_obj_t * parent, uint32_t page_count)
{
    lv_obj_t * footer = lv_obj_create(parent);
    lv_obj_remove_style_all(footer);
    lv_obj_set_size(footer, lv_pct(100), 44);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(footer, 8, 0);

    lv_obj_t * previous = lv_button_create(footer);
    lv_obj_set_size(previous, 60, 36);
    lv_obj_set_style_bg_color(previous, lv_color_hex(0x4c4965), 0);
    lv_obj_add_event_cb(previous, page_click_event_cb, LV_EVENT_CLICKED,
                        (void *)&page_previous_event);
    lv_obj_t * previous_label = lv_label_create(previous);
    lv_label_set_text(previous_label, LV_SYMBOL_LEFT);
    lv_obj_center(previous_label);

    lv_obj_t * page_label = lv_label_create(footer);
    lv_obj_set_style_text_font(page_label, font_small, 0);
    lv_obj_set_style_text_color(page_label, lv_color_white(), 0);
    lv_label_set_text_fmt(page_label, "%" LV_PRIu32 " / %" LV_PRIu32,
                          page_index + 1, page_count);

    lv_obj_t * next = lv_button_create(footer);
    lv_obj_set_size(next, 60, 36);
    lv_obj_set_style_bg_color(next, lv_color_hex(0x4c4965), 0);
    lv_obj_add_event_cb(next, page_click_event_cb, LV_EVENT_CLICKED,
                        (void *)&page_next_event);
    lv_obj_t * next_label = lv_label_create(next);
    lv_label_set_text(next_label, LV_SYMBOL_RIGHT);
    lv_obj_center(next_label);
}

static void render_page(uint32_t page)
{
    if(list == NULL || !lv_obj_is_valid(list)) {
        return;
    }

    const uint32_t total_tracks = lv_demo_music_get_track_count();
    lv_obj_clean(list);

    if(total_tracks == 0) {
        page_index = 0;
        page_first_track = 0;
        page_track_count = 0;
        return;
    }

    const uint32_t page_count = 1U + (total_tracks - 1U) / MUSIC_LIST_PAGE_SIZE;
    page_index = page % page_count;
    page_first_track = page_index * MUSIC_LIST_PAGE_SIZE;
    const uint32_t remaining = total_tracks - page_first_track;
    page_track_count = (remaining < MUSIC_LIST_PAGE_SIZE) ?
                       remaining : MUSIC_LIST_PAGE_SIZE;

    for(uint32_t offset = 0; offset < page_track_count; offset++) {
        add_list_button(list, page_first_track + offset);
    }

    if(page_count > 1) {
        add_page_footer(list, page_count);
    }

    if(selected_track_id >= page_first_track &&
       selected_track_id < page_first_track + page_track_count) {
        lv_obj_t * selected = lv_obj_get_child(list, selected_track_id - page_first_track);
        if(selected != NULL) {
            lv_obj_add_state(selected, LV_STATE_CHECKED);
            lv_obj_t * icon = lv_obj_get_child(selected, 0);
            if(icon != NULL) {
                lv_image_set_src(icon, &img_lv_demo_music_btn_list_pause);
            }
        }
    }
}

static void page_change_async_cb(void * user_data)
{
    render_page((uint32_t)(uintptr_t)user_data);
}

static void page_click_event_cb(lv_event_t * e)
{
    const uint32_t track_count = lv_demo_music_get_track_count();
    if(track_count == 0) {
        return;
    }

    const uint32_t page_count = 1U + (track_count - 1U) / MUSIC_LIST_PAGE_SIZE;
    uint32_t requested_page = page_index;
    if(lv_event_get_user_data(e) == &page_previous_event) {
        requested_page = (page_index == 0) ? page_count - 1U : page_index - 1U;
    }
    else {
        requested_page = (page_index + 1U) % page_count;
    }

    /* Rebuilding the page deletes the clicked arrow. Defer it until LVGL has
     * finished dispatching the current event to avoid a target use-after-free. */
    lv_async_call(page_change_async_cb, (void *)(uintptr_t)requested_page);
}

static void btn_click_event_cb(lv_event_t * e)
{
    uint32_t idx = (uint32_t)(uintptr_t)lv_event_get_user_data(e);

    lv_demo_music_play(idx);
}

static void list_delete_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if(code == LV_EVENT_DELETE) {
        list = NULL;
        lv_style_reset(&style_scrollbar);
        lv_style_reset(&style_btn);
        lv_style_reset(&style_button_pr);
        lv_style_reset(&style_button_chk);
        lv_style_reset(&style_button_dis);
        lv_style_reset(&style_title);
        lv_style_reset(&style_artist);
        lv_style_reset(&style_time);
    }
}
void lv_demo_music_list_close(void)
{
    lv_style_reset(&style_scrollbar);
    lv_style_reset(&style_btn);
    lv_style_reset(&style_button_pr);
    lv_style_reset(&style_button_chk);
    lv_style_reset(&style_button_dis);
    lv_style_reset(&style_title);
    lv_style_reset(&style_artist);
    lv_style_reset(&style_time);
}


#endif /*LV_USE_DEMO_MUSIC*/
