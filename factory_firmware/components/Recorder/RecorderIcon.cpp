/*
 * SPDX-FileCopyrightText: 2026 Waveshare
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <array>
#include <cstddef>
#include <cstdint>

#include "lvgl.h"

namespace {

constexpr int ICON_SIZE = 112;
constexpr size_t ICON_BYTES = ICON_SIZE * ICON_SIZE * 4;

constexpr bool insideRoundedRectangle(
    int x,
    int y,
    int left,
    int top,
    int right,
    int bottom,
    int radius
)
{
    if (x < left || x > right || y < top || y > bottom) {
        return false;
    }
    const int center_x =
        x < left + radius ? left + radius : (x > right - radius ? right - radius : x);
    const int center_y =
        y < top + radius ? top + radius : (y > bottom - radius ? bottom - radius : y);
    const int delta_x = x - center_x;
    const int delta_y = y - center_y;
    return delta_x * delta_x + delta_y * delta_y <= radius * radius;
}

constexpr bool insideCircle(int x, int y, int center_x, int center_y, int radius)
{
    const int delta_x = x - center_x;
    const int delta_y = y - center_y;
    return delta_x * delta_x + delta_y * delta_y <= radius * radius;
}

constexpr bool insideMicrophoneStand(int x, int y)
{
    const bool left_arm = insideRoundedRectangle(x, y, 24, 42, 33, 61, 5);
    const bool right_arm = insideRoundedRectangle(x, y, 79, 42, 88, 61, 5);

    const int delta_x = x - 56;
    const int delta_y = y - 56;
    const int distance_squared = delta_x * delta_x + delta_y * delta_y;
    const bool lower_arc = y >= 56 && distance_squared <= 32 * 32 &&
                           distance_squared >= 24 * 24;

    const bool stem = insideRoundedRectangle(x, y, 51, 84, 61, 97, 5);
    const bool base = insideRoundedRectangle(x, y, 33, 94, 79, 103, 5);
    return left_arm || right_arm || lower_arc || stem || base;
}

constexpr std::array<uint8_t, ICON_BYTES> makeRecorderIcon()
{
    std::array<uint8_t, ICON_BYTES> pixels{};

    for (int y = 0; y < ICON_SIZE; ++y) {
        for (int x = 0; x < ICON_SIZE; ++x) {
            uint8_t red = 0;
            uint8_t green = 0;
            uint8_t blue = 0;
            uint8_t alpha = 0;

            if (insideRoundedRectangle(x, y, 2, 2, 109, 109, 27)) {
                // A warm recorder-red to violet gradient keeps this icon in the
                // same saturated, rounded-square family as the media apps.
                red = static_cast<uint8_t>(239 - (y * 73) / (ICON_SIZE - 1));
                green = static_cast<uint8_t>(45 + ((x + y) * 21) / (2 * (ICON_SIZE - 1)));
                blue = static_cast<uint8_t>(101 + (x * 105) / (ICON_SIZE - 1));
                alpha = 255;

                // A quiet radial glow separates the white microphone from the
                // gradient without adding text or font-dependent symbols.
                const int glow_x = x - 53;
                const int glow_y = y - 47;
                const int glow_distance = glow_x * glow_x + glow_y * glow_y;
                if (glow_distance <= 46 * 46) {
                    const int glow = (46 * 46 - glow_distance) * 18 / (46 * 46);
                    red = static_cast<uint8_t>(red + ((255 - red) * glow) / 100);
                    green = static_cast<uint8_t>(green + ((255 - green) * glow) / 100);
                    blue = static_cast<uint8_t>(blue + ((255 - blue) * glow) / 100);
                }
            }

            // Compact shadows retain the microphone silhouette against every
            // part of the diagonal background gradient.
            if (insideMicrophoneStand(x - 3, y - 3) ||
                    insideRoundedRectangle(x - 3, y - 3, 37, 14, 75, 72, 19)) {
                red = static_cast<uint8_t>((red * 45 + 105 * 55) / 100);
                green = static_cast<uint8_t>((green * 45 + 31 * 55) / 100);
                blue = static_cast<uint8_t>((blue * 45 + 112 * 55) / 100);
                alpha = 255;
            }

            if (insideMicrophoneStand(x, y)) {
                red = 250;
                green = 247;
                blue = 255;
                alpha = 255;
            }

            if (insideRoundedRectangle(x, y, 37, 14, 75, 72, 19)) {
                const int body_y = y < 14 ? 0 : (y > 72 ? 58 : y - 14);
                red = static_cast<uint8_t>(255 - body_y / 7);
                green = static_cast<uint8_t>(253 - body_y / 5);
                blue = 255;
                alpha = 255;
            }

            // Three recessed grille slots make the glyph read as a microphone
            // even when the launcher scales it down.
            if (insideRoundedRectangle(x, y, 46, 28, 66, 33, 3) ||
                    insideRoundedRectangle(x, y, 46, 40, 66, 45, 3) ||
                    insideRoundedRectangle(x, y, 46, 52, 66, 57, 3)) {
                red = 211;
                green = 76;
                blue = 151;
                alpha = 255;
            }

            // A high-contrast recording lamp replaces emoji/text badges and is
            // still legible in the target launcher grid.
            if (insideCircle(x, y, 87, 23, 10)) {
                red = 255;
                green = 197;
                blue = 69;
                alpha = 255;
            }
            if (insideCircle(x, y, 83, 19, 3)) {
                red = 255;
                green = 246;
                blue = 205;
                alpha = 255;
            }

            const size_t offset = static_cast<size_t>(y * ICON_SIZE + x) * 4;
            pixels[offset + 0] = blue;
            pixels[offset + 1] = green;
            pixels[offset + 2] = red;
            pixels[offset + 3] = alpha;
        }
    }

    return pixels;
}

alignas(4) constexpr auto RECORDER_ICON_PIXELS = makeRecorderIcon();

} // namespace

extern const lv_image_dsc_t img_app_recorder = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_ARGB8888,
        .flags = 0,
        .w = ICON_SIZE,
        .h = ICON_SIZE,
        .stride = ICON_SIZE * 4,
        .reserved_2 = 0,
    },
    .data_size = RECORDER_ICON_PIXELS.size(),
    .data = RECORDER_ICON_PIXELS.data(),
    .reserved = nullptr,
    .reserved_2 = nullptr,
};
