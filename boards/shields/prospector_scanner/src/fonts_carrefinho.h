/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Font declarations for Operator layout
 * Original fonts: https://github.com/carrefinho/prospector-zmk-module
 */

#pragma once

#include <lvgl.h>

/* Operator and Radii layout fonts */
LV_FONT_DECLARE(FG_Medium_20);              /* Modifiers, USB/BLE labels, slot labels */
LV_FONT_DECLARE(FR_Medium_32);              /* WPM label */
LV_FONT_DECLARE(DINishExpanded_Light_36);   /* Layer name */
LV_FONT_DECLARE(DINish_Medium_24);          /* Battery labels */
LV_FONT_DECLARE(DINishCondensed_SemiBold_22); /* Radii modifier labels (text mode) */
LV_FONT_DECLARE(Symbols_Semibold_32);       /* Radii modifier symbols (Mac mode) */

/* Mac-style modifier symbols (from carrefinho's symbols.h) */
#define SYMBOL_COMMAND "\xF4\x80\x86\x94"   /* ⌘ */
#define SYMBOL_OPTION "\xF4\x80\x86\x95"    /* ⌥ */
#define SYMBOL_CONTROL "\xF4\x80\x86\x8D"   /* ⌃ */
#define SYMBOL_SHIFT "\xF4\x80\x86\x9D"     /* ⇧ */
#define SYMBOL_SHIFT_FILLED "\xF4\x80\x86\x9E" /* ⇧ filled */
