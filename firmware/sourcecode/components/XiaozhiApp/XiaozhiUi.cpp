/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "XiaozhiUi.hpp"

#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "cbin_font.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "font_awesome.h"
#include "font_emoji.h"

LV_FONT_DECLARE(font_puhui_basic_30_4);
LV_FONT_DECLARE(font_awesome_30_4);

namespace esp_brookesia::apps {

namespace {

constexpr char TAG[] = "XiaozhiUi";
constexpr char COMMON_FONT_PATH[] =
    BSP_SPIFFS_MOUNT_POINT "/xiaozhi/font.bin";
constexpr size_t MAX_COMMON_FONT_SIZE = 4 * 1024 * 1024;
constexpr int TOP_BAR_HEIGHT = 54;
constexpr int SUBTITLE_SINGLE_HEIGHT = 62;
constexpr int SUBTITLE_MULTILINE_HEIGHT = 132;
constexpr int ROUND_DISPLAY_MAX_SIZE = 480;

struct EmotionGlyph {
    const char *name;
    const char *utf8;
};

constexpr EmotionGlyph EMOTION_GLYPHS[] = {
    {"neutral", "\xF0\x9F\x98\xB6"},
    {"happy", "\xF0\x9F\x99\x82"},
    {"laughing", "\xF0\x9F\x98\x86"},
    {"funny", "\xF0\x9F\x98\x82"},
    {"sad", "\xF0\x9F\x98\x94"},
    {"angry", "\xF0\x9F\x98\xA0"},
    {"crying", "\xF0\x9F\x98\xAD"},
    {"loving", "\xF0\x9F\x98\x8D"},
    {"embarrassed", "\xF0\x9F\x98\xB3"},
    {"surprised", "\xF0\x9F\x98\xAF"},
    {"shocked", "\xF0\x9F\x98\xB1"},
    {"thinking", "\xF0\x9F\xA4\x94"},
    {"winking", "\xF0\x9F\x98\x89"},
    {"cool", "\xF0\x9F\x98\x8E"},
    {"relaxed", "\xF0\x9F\x98\x8C"},
    {"delicious", "\xF0\x9F\xA4\xA4"},
    {"kissy", "\xF0\x9F\x98\x98"},
    {"confident", "\xF0\x9F\x98\x8F"},
    {"sleepy", "\xF0\x9F\x98\xB4"},
    {"silly", "\xF0\x9F\x98\x9C"},
    {"confused", "\xF0\x9F\x99\x84"},
};

void prepareTransparentObject(lv_obj_t *object)
{
    lv_obj_remove_style_all(object);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(object, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(object, 0, LV_PART_MAIN);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

void setLabelTextIfChanged(lv_obj_t *label, const char *text)
{
    if (!label) {
        return;
    }
    const char *value = text ? text : "";
    const char *current = lv_label_get_text(label);
    if (!current || strcmp(current, value) != 0) {
        lv_label_set_text(label, value);
    }
}

void setHidden(lv_obj_t *object, bool hidden)
{
    if (!object || lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN) == hidden) {
        return;
    }

    if (hidden) {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

} // namespace

XiaozhiUi::~XiaozhiUi()
{
    destroy();
    _emoji_font = nullptr;
    releaseTextFont();
    _text_font = &font_puhui_basic_30_4;
}

bool XiaozhiUi::preload()
{
    _text_font = &font_puhui_basic_30_4;
    if (_cbin_font) {
        _text_font = _cbin_font;
    } else {
        (void)loadTextFont();
    }
    if (!_emoji_font) {
        if (bsp_display_lock(0)) {
            _emoji_font = font_emoji_64_init();
            bsp_display_unlock();
        } else {
            ESP_LOGW(TAG, "Unable to lock LVGL while loading the emoji font");
        }
    }
    return true;
}

bool XiaozhiUi::create(
    lv_obj_t *parent,
    ActionCallback action_callback,
    void *action_context,
    bool multiline_subtitle
)
{
    if (!parent) {
        return false;
    }

    destroyView();
    (void)action_callback;
    (void)action_context;
    _text_font = _cbin_font ? _cbin_font : &font_puhui_basic_30_4;

    lv_obj_update_layout(parent);
    const lv_coord_t parent_width = lv_obj_get_width(parent);
    const lv_coord_t parent_height = lv_obj_get_height(parent);
    const bool compact_round =
        parent_width > 0 && parent_height > 0 &&
        parent_width <= ROUND_DISPLAY_MAX_SIZE &&
        parent_height <= ROUND_DISPLAY_MAX_SIZE &&
        (parent_width > parent_height ? parent_width - parent_height :
                                        parent_height - parent_width) <= 8;
    // Keep the compact layout for small round displays only. The target's
    // 410 x 502 rectangular display uses the regular status-bar layout.
    const int top_bar_height = compact_round ? 80 : TOP_BAR_HEIGHT;
    const int subtitle_height = compact_round ?
        (multiline_subtitle ? 108 : 60) :
        (multiline_subtitle ? SUBTITLE_MULTILINE_HEIGHT :
                              SUBTITLE_SINGLE_HEIGHT);

    _root = lv_obj_create(parent);
    prepareTransparentObject(_root);
    lv_obj_set_size(_root, lv_pct(100), lv_pct(100));
    lv_obj_align(_root, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(_root, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(_root, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_text_font(_root, _text_font, LV_PART_MAIN);

    _top_bar = lv_obj_create(_root);
    prepareTransparentObject(_top_bar);
    lv_obj_set_size(_top_bar, lv_pct(100), top_bar_height);
    lv_obj_align(_top_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(_top_bar, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_top_bar, LV_OPA_50, LV_PART_MAIN);

    _network_label = lv_label_create(_top_bar);
    lv_obj_set_style_text_font(_network_label, &font_awesome_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(_network_label, lv_color_hex(0x6B6B6B), LV_PART_MAIN);
    lv_obj_align(
        _network_label,
        LV_ALIGN_LEFT_MID,
        compact_round ? 132 : 16,
        0
    );

    _status_label = lv_label_create(_top_bar);
    lv_obj_set_width(_status_label, lv_pct(compact_round ? 42 : 74));
    lv_label_set_long_mode(_status_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(_status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(_status_label, _text_font, LV_PART_MAIN);
    lv_obj_align(_status_label, LV_ALIGN_CENTER, 0, 0);

    _emoji_box = lv_obj_create(_root);
    prepareTransparentObject(_emoji_box);
    lv_obj_set_size(_emoji_box, compact_round ? 88 : 96, compact_round ? 88 : 96);
    lv_obj_align(_emoji_box, LV_ALIGN_CENTER, 0, compact_round ? -4 : -18);

    _emoji_label = lv_label_create(_emoji_box);
    lv_obj_set_style_text_color(_emoji_label, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_center(_emoji_label);

    _activation_box = lv_obj_create(_root);
    prepareTransparentObject(_activation_box);
    lv_obj_set_size(
        _activation_box,
        lv_pct(compact_round ? 78 : 88),
        compact_round ? 238 : 250
    );
    lv_obj_set_flex_flow(_activation_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(
        _activation_box,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER
    );
    lv_obj_set_style_pad_row(_activation_box, compact_round ? 12 : 18, LV_PART_MAIN);
    lv_obj_align(_activation_box, LV_ALIGN_CENTER, 0, compact_round ? -4 : -18);
    lv_obj_add_flag(_activation_box, LV_OBJ_FLAG_HIDDEN);

    _activation_code_label = lv_label_create(_activation_box);
    lv_obj_set_width(_activation_code_label, lv_pct(100));
    lv_obj_set_style_text_align(_activation_code_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(_activation_code_label, _text_font, LV_PART_MAIN);

    _activation_message_label = lv_label_create(_activation_box);
    lv_obj_set_width(_activation_message_label, lv_pct(100));
    lv_obj_set_height(_activation_message_label, compact_round ? 142 : 150);
    lv_label_set_long_mode(_activation_message_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(_activation_message_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(_activation_message_label, _text_font, LV_PART_MAIN);
    lv_obj_set_style_text_color(
        _activation_message_label,
        lv_color_hex(0x5A5A5A),
        LV_PART_MAIN
    );

    _subtitle_bar = lv_obj_create(_root);
    prepareTransparentObject(_subtitle_bar);
    lv_obj_set_size(
        _subtitle_bar,
        lv_pct(compact_round ? 64 : 100),
        subtitle_height
    );
    lv_obj_align(
        _subtitle_bar,
        LV_ALIGN_BOTTOM_MID,
        0,
        compact_round ? -60 : 0
    );
    lv_obj_set_style_bg_color(_subtitle_bar, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_subtitle_bar, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(_subtitle_bar, compact_round ? 12 : 16, LV_PART_MAIN);
    lv_obj_set_style_radius(_subtitle_bar, compact_round ? 20 : 0, LV_PART_MAIN);
    lv_obj_add_flag(_subtitle_bar, LV_OBJ_FLAG_HIDDEN);

    _subtitle_label = lv_label_create(_subtitle_bar);
    lv_obj_set_width(_subtitle_label, lv_pct(100));
    if (multiline_subtitle) {
        lv_obj_set_height(_subtitle_label, subtitle_height - 20);
    }
    lv_label_set_long_mode(
        _subtitle_label,
        multiline_subtitle ? LV_LABEL_LONG_MODE_WRAP : LV_LABEL_LONG_MODE_SCROLL_CIRCULAR
    );
    lv_obj_set_style_text_align(_subtitle_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(_subtitle_label, _text_font, LV_PART_MAIN);
    lv_obj_align(_subtitle_label, LV_ALIGN_CENTER, 0, 0);

    setNetworkReady(false);
    setStatus("");
    setEmotion("neutral");
    clearChatMessages();
    return true;
}

void XiaozhiUi::destroy()
{
    destroyView();
}

void XiaozhiUi::destroyView()
{
    if (_root) {
        lv_obj_delete(_root);
    }

    _root = nullptr;
    _top_bar = nullptr;
    _network_label = nullptr;
    _status_label = nullptr;
    _emoji_box = nullptr;
    _emoji_label = nullptr;
    _activation_box = nullptr;
    _activation_code_label = nullptr;
    _activation_message_label = nullptr;
    _subtitle_bar = nullptr;
    _subtitle_label = nullptr;
    _network_state_valid = false;
    _network_ready = false;
    _activation_state_valid = false;
    _activation_visible = false;
    _emotion_name[0] = '\0';
}

void XiaozhiUi::setNetworkReady(bool ready)
{
    if (!_network_label) {
        return;
    }
    if (_network_state_valid && _network_ready == ready) {
        return;
    }
    lv_label_set_text(
        _network_label,
        ready ? FONT_AWESOME_WIFI : FONT_AWESOME_WIFI_SLASH
    );
    lv_obj_set_style_text_color(
        _network_label,
        lv_color_hex(ready ? 0x202020 : 0x8A8A8A),
        LV_PART_MAIN
    );
    _network_ready = ready;
    _network_state_valid = true;
}

void XiaozhiUi::setStatus(const char *status)
{
    setLabelTextIfChanged(_status_label, status);
}

void XiaozhiUi::setChatMessage(const char *role, const char *text)
{
    (void)role;
    if (!_subtitle_label || !_subtitle_bar) {
        return;
    }

    const char *value = text ? text : "";
    setLabelTextIfChanged(_subtitle_label, value);
    setHidden(_subtitle_bar, value[0] == '\0');
}

void XiaozhiUi::clearChatMessages()
{
    setChatMessage("system", "");
}

void XiaozhiUi::setEmotion(const char *emotion)
{
    if (!_emoji_label) {
        return;
    }

    const char *name = emotion && emotion[0] ? emotion : "neutral";
    if (strcmp(_emotion_name, name) == 0) {
        return;
    }

    const char *emoji = emojiForEmotion(name);
    if (_emoji_font && emoji) {
        lv_obj_set_style_text_font(_emoji_label, _emoji_font, LV_PART_MAIN);
        lv_label_set_text(_emoji_label, emoji);
        snprintf(_emotion_name, sizeof(_emotion_name), "%s", name);
        return;
    }

    const char *icon = font_awesome_get_utf8(name);
    lv_obj_set_style_text_font(_emoji_label, &font_awesome_30_4, LV_PART_MAIN);
    lv_label_set_text(_emoji_label, icon ? icon : FONT_AWESOME_MICROCHIP_AI);
    snprintf(_emotion_name, sizeof(_emotion_name), "%s", name);
}

void XiaozhiUi::setActivation(const char *code, const char *message, bool visible)
{
    if (!_activation_box || !_emoji_box) {
        return;
    }

    setLabelTextIfChanged(_activation_code_label, code);
    setLabelTextIfChanged(_activation_message_label, message);
    if (!_activation_state_valid || _activation_visible != visible) {
        setHidden(_emoji_box, visible);
        setHidden(_activation_box, !visible);
        _activation_visible = visible;
        _activation_state_valid = true;
    }
}

bool XiaozhiUi::loadTextFont()
{
    if (_cbin_font) {
        _text_font = _cbin_font;
        return true;
    }
    FILE *file = fopen(COMMON_FONT_PATH, "rb");
    if (!file) {
        ESP_LOGW(TAG, "Common font is unavailable: %s", COMMON_FONT_PATH);
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long file_size = ftell(file);
    if (file_size <= 0 || static_cast<size_t>(file_size) > MAX_COMMON_FONT_SIZE ||
            fseek(file, 0, SEEK_SET) != 0) {
        ESP_LOGW(TAG, "Common font has an invalid size: %ld", file_size);
        fclose(file);
        return false;
    }

    size_t size = static_cast<size_t>(file_size);
    auto *data = static_cast<uint8_t *>(
                     heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                 );
    if (!data) {
        ESP_LOGW(TAG, "No PSRAM for common font (%u bytes)", static_cast<unsigned>(size));
        fclose(file);
        return false;
    }

    size_t read_size = fread(data, 1, size, file);
    fclose(file);
    if (read_size != size) {
        ESP_LOGW(TAG, "Common font read was incomplete (%u/%u)",
                 static_cast<unsigned>(read_size), static_cast<unsigned>(size));
        heap_caps_free(data);
        return false;
    }

    if (!bsp_display_lock(0)) {
        ESP_LOGW(TAG, "Unable to lock LVGL while decoding the common font");
        heap_caps_free(data);
        return false;
    }
    lv_font_t *font = cbin_font_create(data);
    bsp_display_unlock();

    if (!font) {
        ESP_LOGW(TAG, "Common font could not be decoded");
        heap_caps_free(data);
        return false;
    }

    _font_data = data;
    _cbin_font = font;
    _text_font = font;
    ESP_LOGI(TAG, "Loaded common font from PSRAM (%u bytes)", static_cast<unsigned>(size));
    return true;
}

void XiaozhiUi::releaseTextFont()
{
    if (_cbin_font && !bsp_display_lock(0)) {
        ESP_LOGE(TAG, "Unable to lock LVGL while releasing the common font");
        return;
    }
    if (_cbin_font) {
        cbin_font_delete(_cbin_font);
        _cbin_font = nullptr;
        bsp_display_unlock();
    }
    if (_font_data) {
        heap_caps_free(_font_data);
        _font_data = nullptr;
    }
}

const char *XiaozhiUi::emojiForEmotion(const char *emotion)
{
    if (!emotion) {
        return nullptr;
    }
    for (const auto &entry : EMOTION_GLYPHS) {
        if (strcmp(entry.name, emotion) == 0) {
            return entry.utf8;
        }
    }
    return nullptr;
}

} // namespace esp_brookesia::apps
