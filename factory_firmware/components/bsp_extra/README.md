# `bsp_extra` board integration

[简体中文](README_ZH.md)

`bsp_extra` is the firmware-local integration layer above the managed
ESP32-S3-Touch-AMOLED-2.06 BSP. It provides the common ES8311/ES7210 codec
session used by MusicPlayer, VideoPlayer, Recorder, SpecAnalyzer, and Xiaozhi,
plus the file-player adapter required by MusicPlayer.

Keep generally reusable board support in the Waveshare managed BSP. Keep code
here only when it coordinates this Brookesia product's applications or lifecycle.

## Audio topology

The current managed BSP supplies one shared I2S controller and codec data
interface:

- ES8311 playback uses standard stereo I2S.
- ES7210 capture uses four-slot TDM when voice/capture mode is selected.
- The ES7210 serialized slot order is MIC1, MIC3/reference, MIC2, MIC4.
- TDM slot masks select serialized slots returned by reads. Physical microphone
  masks select ES7210 inputs for gain control. They are different namespaces
  and must not be interchanged.

`bsp_extra_codec_init()` initializes the BSP-owned mixed standard-I2S TX and
TDM RX path, obtains the BSP-owned speaker and microphone codec handles, and
opens the default 16 kHz, 16-bit stereo playback format. The two format helpers
then provide the supported operating modes:

- `bsp_extra_codec_set_fs()` closes the current codec streams and reopens
  ES8311 playback for the requested sample rate, bit width, and mono/stereo mode.
- `bsp_extra_codec_set_voice_fs()` closes the current streams, opens ES8311
  stereo playback, opens ES7210 capture with the selected channel count and TDM
  slots, and applies gain to the selected physical microphone inputs.

The read and write adapters call `esp_codec_dev_read()` and
`esp_codec_dev_write()`. A successful call reports the complete requested byte
count. The current codec API is synchronous, so the adapter's `timeout_ms`
argument is accepted for callback compatibility but is not independently
enforced here.

## Current BSP pin mapping

The values below come from the resolved managed BSP and are consumed through
BSP macros; do not duplicate them as new literals in applications.

| Signal | GPIO |
| --- | ---: |
| I2S MCLK | 42 |
| I2S BCLK/SCLK | 9 |
| I2S LRCK/WS | 45 |
| ESP32-S3 data out to ES8311 | 8 |
| ES7210 data in to ESP32-S3 | 10 |
| Power-amplifier enable | 46 |

The PA enable is active high and is configured by the codec GPIO interface.
`bsp_extra_codec_pa_is_enabled()` does not change the output state: it enables
the GPIO input buffer and reads the physical pad level so logs and diagnostics
can report whether the PA is actually asserted.

## Exclusive audio ownership

The ES8311, ES7210, I2S controller, clocks, and PA form one shared session.
`bsp_extra_audio_session_acquire()` therefore implements a small cross-core-safe
owner token for these clients:

- MusicPlayer
- VideoPlayer
- Recorder
- SpecAnalyzer
- Xiaozhi

Acquisition is non-blocking and non-recursive. It returns `ESP_OK` only when the
previous owner is `NONE`; otherwise it returns `ESP_ERR_INVALID_STATE` and the
caller must report busy or retry later. Only the current owner can release the
session, and every successful acquire requires exactly one matching release.
An invalid, duplicate, or cross-owner release is rejected.

The owner token protects the complete codec lifecycle, not one read, write, or
play request. A client must follow this order:

1. Acquire its owner token.
2. Initialize or reconfigure the codec and start its worker or file operation.
3. Stop callbacks and workers, then close every file or SD-backed resource.
4. Delete the player or stop the codec streams and confirm cleanup succeeded.
5. Release the audio owner token.

Do not release the owner while a decoder, recorder, network callback, or queued
file can still reach the codec. If teardown returns an error, keep ownership
and finish or retry cleanup instead of making the hardware available to a
second application.

## File-player lifecycle

`bsp_extra_player_init()` initializes the codec, resumes the default playback
format, creates the audio-player worker on core 0 at priority 5, and registers
the forwarding callback. File iteration accepts case-insensitive `.mp3` and
`.wav` names only.

After a successful `audio_player_play()` handoff, the audio player owns the
`FILE` and closes it. If handoff fails, the caller remains responsible for
closing the file. `bsp_extra_player_del()` first waits for the overridden audio
player to delete safely, then clears local state and closes capture before
playback. Callers must not release an SD mount lease or the audio owner until
this function succeeds.

## Validation

Audio changes require the MusicPlayer, Recorder, SpecAnalyzer, VideoPlayer, and
AIChats switching checks in
[`../../HARDWARE_VALIDATION.md`](../../HARDWARE_VALIDATION.md). Confirm the PA
log reports the expected state, both ES7210 microphone channels carry data, and
rapid application exit does not leave an audio owner or SD file behind.
