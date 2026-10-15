/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "file_iterator.h"
#include "storage_service.h"
#include "storage_service_ui.hpp"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class MusicPlayer: public systems::phone::App {
public:
    static MusicPlayer *requestInstance(bool use_status_bar = false, bool use_navigation_bar = false);
    ~MusicPlayer();

protected:
    MusicPlayer(bool use_status_bar, bool use_navigation_bar);

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;
    bool deinit(void) override;
    bool pause(void) override;
    bool resume(void) override;

private:
    static MusicPlayer *_instance;

    file_iterator_instance_t *_file_iterator;
    storage_service_lease_t _storage_lease;
    bool _player_initialized;
    bool _audio_session_acquired;
    bool _demo_created;
    brookesia::storage_ui::SdCardGate _sd_gate;

    bool startAudioSession(void);
    bool stopAudioSession(bool request_pause);
};

} // namespace esp_brookesia::apps
