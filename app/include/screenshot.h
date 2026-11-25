#pragma once

// Screenshot over the console: the shell command "shot" asks for one, tools/screenshot.py reads it.
// Call from the LVGL thread before lv_timer_handler(): starts a pending screenshot (full redraw).
void screenshot_process(void);
