# MP3 stream recovery

This project replaces only `audio_mp3.cpp` from the pinned
`chmorgan/esp-audio-player` 1.1.0 dependency. `override.cmake` selects the local
source after ESP-IDF creates the component target. Registry files are untouched.
The upstream Apache-2.0 license is included in this directory.

Helix returns `ERR_MP3_INVALID_FRAMEHEADER` (-6) without advancing the input
pointer. The upstream wrapper retries the same bytes indefinitely. ID3v2 cover
art can trigger this because the upstream wrapper scans the tag as audio.

The replacement skips bounded ID3v2 tags (including v2.4 footers), advances past
invalid candidates, preserves split sync/header/frame bytes, and still searches
remaining buffered audio at EOF. Read errors stop decoding instead of being
reported as normal EOF. No PCM is submitted when a frame was not decoded.

Only standard indexed MP3 bitrates are accepted. Free-format candidates
(bitrate index zero) are rejected before calling Helix: its failed free-format
sync path retains an invalid frame size that makes later recovery unsafe.

Review the replacement and run the host regression tests when upgrading the
player or Helix dependency. A host decode test does not validate SD-card reads,
I2S output, or playback on the device.
