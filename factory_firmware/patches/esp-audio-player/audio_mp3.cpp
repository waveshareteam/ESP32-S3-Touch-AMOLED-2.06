// SPDX-License-Identifier: Apache-2.0
// Adapted from chmorgan/esp-audio-player 1.1.0 audio_mp3.cpp.
// Local changes: skip ID3v2 metadata, preserve split frames and always advance
// past invalid frame candidates. See LICENSE for the upstream license.
#include <string.h>
#include "audio_log.h"
#include "audio_mp3.h"

static const char *TAG = "mp3";

namespace {

bool is_layer3_header(const uint8_t *header)
{
    return header[0] == 0xff && (header[1] & 0xe0) == 0xe0 &&
           (header[1] & 0x18) != 0x08 && // Reserved MPEG version.
           (header[1] & 0x06) == 0x02 && // Layer III only.
           // A failed free-format sync leaves invalid state in this Helix
           // version. Accept standard indexed bitrates before calling it.
           (header[2] & 0xf0) != 0 && (header[2] & 0xf0) != 0xf0 &&
           (header[2] & 0x0c) != 0x0c;
}

} // namespace

bool is_mp3(FILE *fp)
{
    if (fseek(fp, 0, SEEK_SET) != 0) {
        return false;
    }

    bool has_id3 = false;
    for (;;) {
        const long start = ftell(fp);
        uint8_t header[10];
        const size_t count = fread(header, 1, sizeof(header), fp);
        if (count < 3 || memcmp(header, "ID3", 3) != 0) {
            // Playback begins after the tags, not inside embedded cover art.
            if (start < 0 || fseek(fp, start, SEEK_SET) != 0) {
                return false;
            }
            return has_id3 || (count >= 4 && is_layer3_header(header));
        }

        if (count != sizeof(header) || header[3] < 2 || header[3] > 4 ||
                header[4] == 0xff ||
                ((header[6] | header[7] | header[8] | header[9]) & 0x80)) {
            ESP_LOGW(TAG, "Invalid ID3v2 header");
            return false;
        }
        const uint32_t tag_size = (uint32_t(header[6]) << 21) |
                                  (uint32_t(header[7]) << 14) |
                                  (uint32_t(header[8]) << 7) | header[9];
        const uint32_t footer_size = header[3] == 4 && (header[5] & 0x10) ? 10 : 0;
        const long payload_start = ftell(fp);
        if (payload_start < 0 || fseek(fp, 0, SEEK_END) != 0) {
            return false;
        }
        const long file_end = ftell(fp);
        if (file_end < payload_start || tag_size + footer_size >
                static_cast<unsigned long>(file_end - payload_start)) {
            ESP_LOGW(TAG, "Truncated ID3v2 tag");
            return false;
        }
        if (fseek(fp, payload_start + tag_size + footer_size, SEEK_SET) != 0) {
            return false;
        }
        ESP_LOGI(TAG, "Skipped ID3v2.%u metadata (%lu bytes)",
                 unsigned(header[3]), static_cast<unsigned long>(tag_size + footer_size + 10));
        has_id3 = true;
    }
}

DECODE_STATUS decode_mp3(HMP3Decoder decoder, FILE *fp, decode_data *data,
                         mp3_instance *instance)
{
    data->frame_count = 0;
    size_t unread = instance->bytes_in_data_buf -
                    (instance->read_ptr - instance->data_buf);

    // More than the largest standard Layer III frame, including side info.
    if (unread < MAINBUF_SIZE * 5 / 4 && !instance->eof_reached) {
        memmove(instance->data_buf, instance->read_ptr, unread);
        const size_t count = fread(instance->data_buf + unread, 1,
                                   instance->data_buf_size - unread, fp);
        instance->bytes_in_data_buf = unread + count;
        instance->read_ptr = instance->data_buf;
        if (ferror(fp)) {
            ESP_LOGE(TAG, "MP3 file read failed");
            return DECODE_STATUS_ERROR;
        }
        instance->eof_reached = count == 0 || feof(fp);
        unread = instance->bytes_in_data_buf;
    }
    if (unread == 0) {
        return DECODE_STATUS_DONE;
    }

    const int offset = MP3FindSyncWord(instance->read_ptr, static_cast<int>(unread));
    if (offset < 0) {
        // A sync word can straddle reads. Preserve its possible leading 0xff.
        const size_t keep = !instance->eof_reached && instance->read_ptr[unread - 1] == 0xff ? 1 : 0;
        instance->read_ptr += unread - keep;
        return instance->eof_reached ? DECODE_STATUS_DONE : DECODE_STATUS_NO_DATA_CONTINUE;
    }
    instance->read_ptr += offset;
    unread -= offset;
    uint8_t *const candidate = instance->read_ptr;

    // Helix reads the header/CRC before checking bytesLeft. Never pass a
    // partial header, even when a damaged file ends with a sync prefix.
    const size_t header_size = (candidate[1] & 1) ? 4 : 6;
    if (unread < header_size) {
        return instance->eof_reached ? DECODE_STATUS_DONE : DECODE_STATUS_NO_DATA_CONTINUE;
    }
    MP3FrameInfo info = {};
    if (!is_layer3_header(candidate) || MP3GetNextFrameInfo(decoder, &info, candidate) != ERR_MP3_NONE) {
        ++instance->read_ptr;
        return DECODE_STATUS_NO_DATA_CONTINUE;
    }

    const size_t side_size = info.version == MPEG1 ? (info.nChans == 1 ? 17 : 32) :
                                                   (info.nChans == 1 ? 9 : 17);
    const size_t required = (info.version == MPEG1 ? 144 : 72) * info.bitrate / info.samprate +
                            ((candidate[2] >> 1) & 1);
    if (required < header_size + side_size || required > instance->data_buf_size) {
        ++instance->read_ptr;
        return DECODE_STATUS_NO_DATA_CONTINUE;
    }
    if (unread < required) {
        if (instance->eof_reached) {
            // A false header near EOF must not hide a later complete frame.
            ++instance->read_ptr;
        }
        return DECODE_STATUS_NO_DATA_CONTINUE;
    }

    uint8_t *next = candidate;
    int bytes_left = static_cast<int>(unread);
    const int error = MP3Decode(decoder, &next, &bytes_left,
                                reinterpret_cast<int16_t *>(data->samples), 0);
    if (error == ERR_MP3_NONE) {
        instance->read_ptr = next;
        MP3GetLastFrameInfo(decoder, &info);
        data->fmt.sample_rate = info.samprate;
        data->fmt.bits_per_sample = info.bitsPerSample;
        data->fmt.channels = info.nChans;
        data->frame_count = info.outputSamps / info.nChans;
        return DECODE_STATUS_CONTINUE;
    }
    if (error == ERR_MP3_OUT_OF_MEMORY || error == ERR_MP3_NULL_POINTER) {
        ESP_LOGE(TAG, "MP3 decoder failed: %d", error);
        return DECODE_STATUS_ERROR;
    }
    if (error == ERR_MP3_INDATA_UNDERFLOW && !instance->eof_reached &&
            unread < MAINBUF_SIZE * 5 / 4) {
        // Refill without losing the partially buffered frame.
        return DECODE_STATUS_NO_DATA_CONTINUE;
    }

    // INVALID_FRAMEHEADER leaves Helix's input pointer unchanged. Retrying
    // that same pointer spins forever. Reservoir underflow normally consumes
    // a frame; retain that progress so the next frame can use the reservoir.
    instance->read_ptr = next > candidate ? next : candidate + 1;
    if (error != ERR_MP3_MAINDATA_UNDERFLOW) {
        ESP_LOGD(TAG, "Skipping invalid MP3 data (decoder error %d)", error);
    }
    return DECODE_STATUS_NO_DATA_CONTINUE;
}
