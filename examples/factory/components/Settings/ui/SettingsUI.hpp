#pragma once

#include "lvgl.h"

namespace esp_brookesia::apps::settings_ui {

static constexpr lv_coord_t PAGE_HEADER_HEIGHT = 72;
// The target's transparent status bar overlays the full-size app screen.
static constexpr lv_coord_t PAGE_STATUS_BAR_SAFE_TOP = 54;
static constexpr lv_coord_t PAGE_HORIZONTAL_MARGIN = 32;
static constexpr lv_coord_t CONTENT_TOP_GAP = 12;
static constexpr lv_coord_t CONTENT_BOTTOM_MARGIN = 20;
static constexpr lv_coord_t ROW_HEIGHT = 84;
static constexpr lv_coord_t CONTROL_PANEL_HEIGHT = 140;
static constexpr int ROUND_SCREEN_TOLERANCE = 8;
static constexpr int ROUND_HORIZONTAL_MARGIN_PERCENT = 15;
static constexpr int ROUND_HEADER_TOP_PERCENT = 10;

static constexpr uint32_t COLOR_BACKGROUND = 0x000000;
static constexpr uint32_t COLOR_SURFACE = 0x242426;
static constexpr uint32_t COLOR_SURFACE_PRESSED = 0x343438;
static constexpr uint32_t COLOR_BORDER = 0x3A3A3C;
static constexpr uint32_t COLOR_PRIMARY_TEXT = 0xFFFFFF;
static constexpr uint32_t COLOR_SECONDARY_TEXT = 0xA7A7AD;
static constexpr uint32_t COLOR_ACCENT = 0x00BFFF;

inline bool is_round_page(lv_obj_t *page)
{
    lv_obj_update_layout(page);
    const lv_coord_t width = lv_obj_get_width(page);
    const lv_coord_t height = lv_obj_get_height(page);
    const lv_coord_t delta = width - height;
    // A fixed Brookesia status bar shortens the app-local screen while the
    // underlying panel remains round. Accept that top-cropped shape too.
    return width >= 300 && height >= 300 && delta >= -ROUND_SCREEN_TOLERANCE &&
           delta <= width / 4;
}

inline lv_coord_t get_horizontal_margin(lv_obj_t *page)
{
    if (!is_round_page(page)) {
        return PAGE_HORIZONTAL_MARGIN;
    }
    return lv_obj_get_width(page) * ROUND_HORIZONTAL_MARGIN_PERCENT / 100;
}

inline lv_coord_t get_header_top(lv_obj_t *page)
{
    if (!is_round_page(page)) {
        return PAGE_STATUS_BAR_SAFE_TOP;
    }
    const lv_coord_t width = lv_obj_get_width(page);
    const lv_coord_t height = lv_obj_get_height(page);
    const lv_coord_t physical_safe_top = width * ROUND_HEADER_TOP_PERCENT / 100;
    const lv_coord_t top_crop = (width > height) ? width - height : 0;
    return (physical_safe_top > top_crop) ? physical_safe_top - top_crop : 0;
}

inline lv_coord_t get_bottom_margin(lv_obj_t *page)
{
    return is_round_page(page) ? get_horizontal_margin(page) : CONTENT_BOTTOM_MARGIN;
}

inline lv_obj_t *create_page(lv_obj_t *screen)
{
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BACKGROUND), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *page = lv_obj_create(screen);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, lv_pct(100), lv_pct(100));
    lv_obj_center(page);
    lv_obj_set_style_bg_color(page, lv_color_hex(COLOR_BACKGROUND), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    return page;
}

inline lv_obj_t *create_header(
    lv_obj_t *page, const char *title, lv_event_cb_t back_cb = nullptr, void *user_data = nullptr)
{
    const bool round_page = is_round_page(page);
    const lv_coord_t horizontal_margin = get_horizontal_margin(page);
    const lv_coord_t header_top = get_header_top(page);

    lv_obj_t *header = lv_obj_create(page);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), PAGE_HEADER_HEIGHT);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, header_top);
    lv_obj_set_style_bg_color(header, lv_color_hex(COLOR_BACKGROUND), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(header, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back_button = nullptr;
    if (back_cb != nullptr) {
        back_button = lv_button_create(header);
        const lv_coord_t back_size = round_page ? 44 : 48;
        const lv_coord_t back_x = round_page ? horizontal_margin + 10 : 20;
        lv_obj_set_size(back_button, back_size, back_size);
        lv_obj_align(back_button, LV_ALIGN_LEFT_MID, back_x, 0);
        lv_obj_set_style_radius(back_button, 8, LV_PART_MAIN);
        lv_obj_set_style_border_width(back_button, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(back_button, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(back_button, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_bg_color(back_button, lv_color_hex(COLOR_SURFACE_PRESSED), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(back_button, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(back_button, back_cb, LV_EVENT_CLICKED, user_data);

        lv_obj_t *back_icon = lv_label_create(back_button);
        lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
        lv_obj_set_style_text_font(back_icon, &lv_font_montserrat_30, LV_PART_MAIN);
        lv_obj_set_style_text_color(back_icon, lv_color_hex(COLOR_PRIMARY_TEXT), LV_PART_MAIN);
        lv_obj_center(back_icon);
    }

    lv_obj_update_layout(page);
    const lv_coord_t title_x = round_page
                               ? ((back_button != nullptr) ? horizontal_margin + 68 : horizontal_margin + 10)
                               : ((back_button != nullptr) ? 84 : PAGE_HORIZONTAL_MARGIN);
    lv_coord_t title_width = lv_obj_get_width(page) - title_x - horizontal_margin;
    if (title_width < 1) {
        title_width = 1;
    }

    lv_obj_t *title_label = lv_label_create(header);
    lv_label_set_text(title_label, title);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(title_label, title_width);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_30, LV_PART_MAIN);
    lv_obj_set_style_text_color(title_label, lv_color_hex(COLOR_PRIMARY_TEXT), LV_PART_MAIN);
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, title_x, 0);

    return back_button;
}

inline lv_obj_t *create_content_list(lv_obj_t *page)
{
    lv_obj_update_layout(page);
    const lv_coord_t horizontal_margin = get_horizontal_margin(page);
    const lv_coord_t content_top = get_header_top(page) + PAGE_HEADER_HEIGHT + CONTENT_TOP_GAP;
    const lv_coord_t bottom_margin = get_bottom_margin(page);
    lv_coord_t content_width = lv_obj_get_width(page) - horizontal_margin * 2;
    lv_coord_t content_height = lv_obj_get_height(page) - content_top - bottom_margin;
    if (content_width < 1) {
        content_width = 1;
    }
    if (content_height < 1) {
        content_height = 1;
    }

    lv_obj_t *list = lv_list_create(page);
    lv_obj_set_size(list, content_width, content_height);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, content_top);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    return list;
}

inline void init_list_styles(
    lv_style_t &list, lv_style_t &row, lv_style_t &section, lv_style_t &pressed)
{
    lv_style_init(&list);
    lv_style_set_pad_all(&list, 0);
    lv_style_set_pad_row(&list, 6);
    lv_style_set_border_width(&list, 0);
    lv_style_set_radius(&list, 0);
    lv_style_set_bg_color(&list, lv_color_hex(COLOR_BACKGROUND));
    lv_style_set_bg_opa(&list, LV_OPA_TRANSP);

    lv_style_init(&row);
    lv_style_set_height(&row, ROW_HEIGHT);
    lv_style_set_pad_hor(&row, 18);
    lv_style_set_pad_ver(&row, 12);
    lv_style_set_pad_column(&row, 14);
    lv_style_set_flex_cross_place(&row, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_track_place(&row, LV_FLEX_ALIGN_CENTER);
    lv_style_set_border_width(&row, 0);
    lv_style_set_radius(&row, 6);
    lv_style_set_bg_color(&row, lv_color_hex(COLOR_SURFACE));
    lv_style_set_bg_opa(&row, LV_OPA_COVER);
    lv_style_set_text_font(&row, &lv_font_montserrat_26);
    lv_style_set_text_color(&row, lv_color_hex(COLOR_PRIMARY_TEXT));

    lv_style_init(&section);
    lv_style_set_text_font(&section, &lv_font_montserrat_20);
    lv_style_set_text_color(&section, lv_color_hex(COLOR_SECONDARY_TEXT));
    lv_style_set_bg_color(&section, lv_color_hex(COLOR_BACKGROUND));
    lv_style_set_bg_opa(&section, LV_OPA_TRANSP);
    lv_style_set_pad_left(&section, 4);
    lv_style_set_pad_right(&section, 4);
    lv_style_set_pad_top(&section, 16);
    lv_style_set_pad_bottom(&section, 4);

    lv_style_init(&pressed);
    lv_style_set_bg_color(&pressed, lv_color_hex(COLOR_SURFACE_PRESSED));
    lv_style_set_bg_opa(&pressed, LV_OPA_COVER);
    lv_style_set_text_color(&pressed, lv_color_hex(COLOR_PRIMARY_TEXT));
}

inline void reset_list_styles(
    lv_style_t &list, lv_style_t &row, lv_style_t &section, lv_style_t &pressed)
{
    lv_style_reset(&list);
    lv_style_reset(&row);
    lv_style_reset(&section);
    lv_style_reset(&pressed);
}

inline lv_obj_t *add_section(lv_obj_t *list, const char *text, lv_style_t &section_style)
{
    lv_obj_t *section = lv_list_add_text(list, text);
    lv_obj_add_style(section, &section_style, LV_PART_MAIN);
    return section;
}

inline lv_obj_t *get_button_label(lv_obj_t *button)
{
    const uint32_t child_count = lv_obj_get_child_count(button);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t *child = lv_obj_get_child(button, i);
        if (lv_obj_has_class(child, &lv_label_class)) {
            return child;
        }
    }
    return nullptr;
}

inline void use_ellipsis_for_button_label(lv_obj_t *button)
{
    lv_obj_t *label = get_button_label(button);
    if (label != nullptr) {
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_flex_grow(label, 1);
    }
}

inline lv_obj_t *add_info_row(
    lv_obj_t *list, const void *icon, const char *name, const char *value, lv_style_t &row_style)
{
    lv_obj_t *row = lv_list_add_button(list, icon, nullptr);
    lv_obj_add_style(row, &row_style, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *name_label = lv_label_create(row);
    lv_label_set_text(name_label, name);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(name_label, 1);
    lv_obj_set_style_text_font(name_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(name_label, lv_color_hex(COLOR_PRIMARY_TEXT), LV_PART_MAIN);

    lv_obj_t *value_label = lv_label_create(row);
    lv_label_set_text(value_label, value);
    lv_label_set_long_mode(value_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(value_label, lv_pct(52));
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_text_font(value_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(value_label, lv_color_hex(COLOR_SECONDARY_TEXT), LV_PART_MAIN);
    return row;
}

} // namespace esp_brookesia::apps::settings_ui
