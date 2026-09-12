/**
 * ============================================================================
 *  display.cpp  -  TFT_eSPI wrapper: sprite, frame buffer, gauges and helpers
 * ============================================================================
 */
#include "display.h"
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
//  Globals
// ---------------------------------------------------------------------------
TFT_eSPI    tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);
uint16_t*   gFrameBuf = nullptr;
uint16_t*   gDimBuf   = nullptr;

// Colour used behind arcs so the anti-aliasing blends into a known value.
#define ARC_BG 0x0000

// ---------------------------------------------------------------------------
//  Lifecycle
// ---------------------------------------------------------------------------
bool displayInit(void) {
  tft.init();
  tft.setRotation(0);
  tft.setSwapBytes(false);
  tft.fillScreen(TFT_BLACK);

  spr.setColorDepth(16);
  if (spr.createSprite(SCR_W, SCR_H) == nullptr) return false;
  spr.setSwapBytes(true);   // raw frames are little endian (see img_to_565.py)

  const size_t fbBytes = (size_t)SCR_W * SCR_H * 2;

  gFrameBuf = (uint16_t*)ps_malloc(fbBytes);
  if (gFrameBuf == nullptr) gFrameBuf = (uint16_t*)malloc(fbBytes);
  if (gFrameBuf == nullptr) return false;
  memset(gFrameBuf, 0, fbBytes);

  gDimBuf = (uint16_t*)ps_malloc(fbBytes);
  if (gDimBuf == nullptr) gDimBuf = (uint16_t*)malloc(fbBytes);
  if (gDimBuf == nullptr) return false;
  memset(gDimBuf, 0, fbBytes);

  return true;
}

void displayPush(void) {
  spr.pushSprite(0, 0);
}

// ---------------------------------------------------------------------------
//  Background dimming: raw frame -> scratch buffer. Keeping gFrameBuf intact
//  means a still animation is not darkened again on every single render.
// ---------------------------------------------------------------------------
void dispDimCopy(uint8_t shift) {
  if (shift < 1) shift = 1;
  const uint16_t mask = (shift >= 2) ? 0xE79C : 0xF7DE;   // 25% or 50% brightness
  const uint16_t* src = gFrameBuf;
  uint16_t*       dst = gDimBuf;
  const uint32_t  n   = (uint32_t)SCR_W * SCR_H;
  for (uint32_t i = 0; i < n; i++) dst[i] = (uint16_t)((src[i] & mask) >> shift);
}

// ---------------------------------------------------------------------------
//  Arc helper - handles a sweep that crosses the 0/360 boundary
// ---------------------------------------------------------------------------
static void arcSeg(int cx, int cy, int rOut, int rIn,
                   int startDeg, int sweepDeg, uint16_t col) {
  if (sweepDeg <= 0) return;
  startDeg = ((startDeg % 360) + 360) % 360;
  int end = startDeg + sweepDeg;
  if (end <= 360) {
    spr.drawSmoothArc(cx, cy, rOut, rIn, startDeg, end, col, ARC_BG, false);
  } else {
    spr.drawSmoothArc(cx, cy, rOut, rIn, startDeg, 360, col, ARC_BG, false);
    spr.drawSmoothArc(cx, cy, rOut, rIn, 0, end - 360, col, ARC_BG, false);
  }
}

void dispGauge(int cx, int cy, int rOut, int rIn,
               int centerDeg, int sweepDeg, int pct,
               uint16_t fill, uint16_t track, bool selected) {
  if (rIn < 0) rIn = 0;

  // 1. the empty track
  arcSeg(cx, cy, rOut, rIn, centerDeg - sweepDeg / 2, sweepDeg, track);

  // 2. the filled portion
  if (pct < 0) return;
  if (pct > 100) pct = 100;
  if (pct == 0) return;
  int fillSweep = (sweepDeg * pct) / 100;
  if (fillSweep < 1) fillSweep = 1;

  // A selected gauge gets a subtle second glow ring underneath
  if (selected && rOut + 3 < SCR_CX)
    arcSeg(cx, cy, rOut + 3, rOut + 1, centerDeg - sweepDeg / 2, sweepDeg, (uint16_t)(track >> 1));

  arcSeg(cx, cy, rOut, rIn, centerDeg - sweepDeg / 2, fillSweep, fill);
}

// ---------------------------------------------------------------------------
//  Rotating ornament for the idle now-playing ticker
// ---------------------------------------------------------------------------
void dispSpinner(int cx, int cy, int rOut, int rIn, int angleDeg, uint16_t col) {
  for (int i = 0; i < 3; i++) {
    arcSeg(cx, cy, rOut, rIn, angleDeg + i * 120, 74, col);
  }
}

// ---------------------------------------------------------------------------
//  Radial text
// ---------------------------------------------------------------------------
void dispRadialText(int cx, int cy, int r, int angleDeg,
                    const char* txt, uint16_t col, uint8_t font) {
  float a = angleDeg * (float)DEG_TO_RAD;
  int x = cx - (int)lroundf((float)r * sinf(a));
  int y = cy + (int)lroundf((float)r * cosf(a));
  dispTextCentre(txt, x, y, font, col);
}

// ---------------------------------------------------------------------------
//  Text helpers (no String allocations anywhere)
// ---------------------------------------------------------------------------
void dispTextCentre(const char* txt, int x, int y, uint8_t font, uint16_t col) {
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(col);
  spr.drawString(txt, x, y, font);
}

void dispTextAt(const char* txt, int x, int y, uint8_t font, uint16_t col) {
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(col);
  spr.drawString(txt, x, y, font);
}

void dispTextCentrePct(int pct, int x, int y, uint8_t font, uint16_t col) {
  char b[8];
  if (pct < 0) strcpy(b, "--");
  else         snprintf(b, sizeof(b), "%d", pct);
  dispTextCentre(b, x, y, font, col);
}

// ---------------------------------------------------------------------------
//  Horizontal bar gauge (used by the detail page rows)
// ---------------------------------------------------------------------------
void dispBar(int x, int y, int w, int h, int pct, uint16_t fill, uint16_t track) {
  spr.fillRoundRect(x, y, w, h, h / 2, track);
  if (pct < 0 || w <= 0) return;
  if (pct > 100) pct = 100;
  int fw = (w * pct) / 100;
  if (fw < h) fw = h;
  spr.fillRoundRect(x, y, fw, h, h / 2, fill);
}

// ---------------------------------------------------------------------------
//  Vector page icons - keeps the UI independent of any extra LittleFS assets
// ---------------------------------------------------------------------------
void dispPageIcon(Page p, int cx, int cy, int size, uint16_t col) {
  const int h = size / 2;

  switch (p) {
    // ---- MONITOR : a gauge with a needle -----------------------------------
    case PAGE_MONITOR: {
      arcSeg(cx, cy + 2, h, h - 4, 45, 270, (uint16_t)(col >> 1));
      arcSeg(cx, cy + 2, h, h - 4, 45, 120, col);
      spr.drawWideLine(cx, cy + 2, cx + h - 5, cy + 2 - (h - 6), 2, col, ARC_BG);
      spr.fillSmoothCircle(cx, cy + 2, 3, col, ARC_BG);
      break;
    }

    // ---- VOLUME : loudspeaker + waves --------------------------------------
    case PAGE_VOLUME: {
      const int bx = cx - h;
      spr.fillRect(bx, cy - 5, 7, 10, col);
      spr.fillTriangle(bx + 7, cy - 5, bx + 7, cy + 5, bx + 15, cy - 13, col);
      spr.fillTriangle(bx + 7, cy - 5, bx + 15, cy - 13, bx + 15, cy + 13, col);
      spr.fillTriangle(bx + 7, cy + 5, bx + 15, cy + 13, bx + 15, cy - 13, col);
      arcSeg(bx + 17, cy, 8, 6, 240, 60, col);
      arcSeg(bx + 17, cy, 13, 11, 240, 60, col);
      break;
    }

    // ---- MEDIA : transport play --------------------------------------------
    case PAGE_MEDIA: {
      spr.drawCircle(cx, cy, h, col);
      spr.fillTriangle(cx - h / 2, cy - h / 2, cx - h / 2, cy + h / 2, cx + h / 2 + 2, cy, col);
      break;
    }

    // ---- GALLERY : picture frame -------------------------------------------
    case PAGE_GALLERY: {
      spr.drawRoundRect(cx - h, cy - h + 2, size, size - 4, 4, col);
      spr.fillTriangle(cx - h + 4, cy + h - 6, cx - 1, cy - 2, cx + 6, cy + h - 6, col);
      spr.fillTriangle(cx + 1, cy + h - 6, cx + 8, cy + 3, cx + h - 4, cy + h - 6, col);
      spr.fillSmoothCircle(cx + h - 8, cy - h + 9, 3, col, ARC_BG);
      break;
    }

    default: break;
  }
}

// ---------------------------------------------------------------------------
//  Toast: a small dark plate with an accent outline and centred text
// ---------------------------------------------------------------------------
void dispToast(const char* txt, uint16_t accent) {
  const int w = 170, h = 34, x = SCR_CX - w / 2, y = SCR_CY - h / 2;
  spr.fillSmoothRoundRect(x, y, w, h, 8, 0x0000, ARC_BG);
  spr.drawRoundRect(x, y, w, h, 8, accent);
  dispTextCentre(txt, SCR_CX, SCR_CY, 2, accent);
}

