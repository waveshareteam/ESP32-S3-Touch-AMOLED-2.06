# xiaozhi-esp32 adaptations

This directory contains the small OGG demuxer imported from
78/xiaozhi-esp32 at commit
0ec696f64f5843ca0f5fcf700ae45977d1dcd2e8.

Optional Chinese activation prompt and digit recordings can be installed in
`/spiffs/xiaozhi/` at image-generation time:

- activation.ogg
- 0.ogg through 9.ogg
- success.ogg

Those recordings are not bundled by this component. When present, they must
be 16 kHz mono Opus audio in OGG containers. The firmware demuxes each file
and feeds raw Opus packets to esp_audio_codec, avoiding a dependency on the
complete upstream audio framework. When they are absent, activation remains
fully usable through the on-screen activation code and message; prompt
playback is simply skipped.

The XiaozhiApp application also adapts the upstream WebSocket protocol,
wake-word Opus pre-roll, automatic listening-turn flow, and dynamic Opus
playback configuration from commit
1b48ebd7863695bf80d384c6c09af6299a6d7d0e. The local integration keeps the
Brookesia application lifecycle, board codec access, dual-microphone AFE,
and UI instead of importing the complete upstream board and application
framework. On ESP32-S3-Touch-AMOLED-2.06, the AFE consumes the ES7210's two
standard-I2S microphone channels. AEC is disabled because the board does not
route an ES8311 playback-reference channel into the ADC stream.

The imported OGG demuxer is distributed under the MIT license in `LICENSE`.
Any optional prompt recordings must be reviewed and licensed separately by
the firmware distributor before they are added to a filesystem image.
