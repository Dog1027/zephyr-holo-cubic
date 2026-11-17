#pragma once

/*
 * The project's color scheme. Every screen takes its colors from here, so the look stays
 * consistent and one edit changes it everywhere; do not write 0xRRGGBB values in an app.
 * All values are 0xRRGGBB: pass them to ui_label() as they are, or wrap them in lv_color_hex().
 *
 * The display is seen through a prism, which costs brightness and contrast: text that has to
 * be read needs THEME_TEXT or THEME_TEXT_SECONDARY. THEME_TEXT_DIM is close to invisible there.
 */

// ---- Surfaces
#define THEME_BG                0x0b0f17    // screen background (ui_screen_init)
#define THEME_BG_RAISED         0x18202e    // a step lighter: the Menu's gradient
#define THEME_TRACK             0x1c2333    // the unfilled part of bars, arcs and spinners
#define THEME_BLACK             0x000000    // behind full screen images (Photos, Anim, Cast)

// ---- Text
#define THEME_TEXT              0xffffff    // values and anything important
#define THEME_TEXT_SECONDARY    0xaab4c8    // explanations that must stay readable
#define THEME_TEXT_DIM          0x5a6478    // titles, units, inactive items: decoration only

// ---- Accents: one per app (its highlight, bar or arc), handed out in this order by the Menu
#define THEME_BLUE              0x4f8ef7
#define THEME_RED               0xf25f5c
#define THEME_GREEN             0x43c59e
#define THEME_ORANGE            0xf5a623
#define THEME_PURPLE            0x9b6cf2
#define THEME_CYAN              0x2ec4d6
#define THEME_YELLOW            0xffc83d

// ---- Meaning: use these when the color says something, whatever the app's accent is
#define THEME_OK                THEME_GREEN
#define THEME_WARNING           THEME_ORANGE
#define THEME_ERROR             THEME_RED
