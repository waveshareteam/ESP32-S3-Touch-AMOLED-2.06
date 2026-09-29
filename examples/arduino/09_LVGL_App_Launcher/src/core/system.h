#pragma once

// Core entry points used by the sketch
void system_setup(void);
void system_loop(void);

// Pushes g_settings (brightness policy, volume, time zone) to the hardware modules
void system_apply_settings(void);
