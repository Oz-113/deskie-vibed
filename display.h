/**
 * ============================================================================
 *  display.h  -  TFT_eSPI wrapper: sprite, frame buffer, gauges and helpers
 * ============================================================================
 *  Owns the panel, the 240x240 compositing sprite and the PSRAM frame buffer.
 *  All drawing primitives used by ui.cpp live here so the UI code stays
 *  declarative and readable.
 * ============================================================================
 */
#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>
#include "state.h"

// ---- globals owned by display.cpp -----------------------------------------
extern TFT_eSPI    tft;
extern TFT_eSprite spr;
extern uint16_t*   gFrameBuf;    // 240*240 RGB565 raw frame (PSRAM)
extern uint16_t*   gDimBuf;      // scratch copy used for the dimmed overlay background

// ---- lifecycle -------------------------------------------------------------
bool     displayInit(void);       // panel + sprite + frame buffer (true on success)
void     displayPush(void);       // blit the sprite to the panel

// ---- frame helpers ---------------------------------------------------------
void     dispDimCopy(uint8_t shift);   // gFrameBuf -> gDimBuf, darkened (overlay background)

// ---- gauges ----------------------------------------------------------------
// Draw a gauge (arc) centred on (cx,cy). Angles follow TFT_eSPI: 0 = 6 o'clock,
// increasing clockwise. 'centerDeg' is the middle of the arc, 'sweepDeg' its
// angular length; wrapping past 0/360 is handled internally.
// 'pct' is 0..100, -1 renders the empty track only (value unavailable).
void dispGauge(int cx, int cy, int rOut, int rIn,
               int centerDeg, int sweepDeg, int pct,
               uint16_t fill, uint16_t track, bool selected);

// Rotating ornament (3 arcs, 120 deg apart) used by the idle now-playing ticker.
void dispSpinner(int cx, int cy, int rOut, int rIn, int angleDeg, uint16_t col);

// Radial label: centred text placed on a circle of radius r at 'angleDeg'.
void dispRadialText(int cx, int cy, int r, int angleDeg,
                    const char* txt, uint16_t col, uint8_t font);

// ---- text ------------------------------------------------------------------
void dispTextCentre(const char* txt, int x, int y, uint8_t font, uint16_t col);
void dispTextCentrePct(int pct, int x, int y, uint8_t font, uint16_t col);
void dispTextAt(const char* txt, int x, int y, uint8_t font, uint16_t col);

// A horizontal "bar gauge" used for the detail rows.
void dispBar(int x, int y, int w, int h, int pct, uint16_t fill, uint16_t track);

// ---- vector page icons (so no extra LittleFS assets are required) ----------
void dispPageIcon(Page p, int cx, int cy, int size, uint16_t col);

// ---- misc ------------------------------------------------------------------
void dispToast(const char* txt, uint16_t accent);      // small centred message
