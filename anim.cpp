/**
 * ============================================================================
 *  anim.cpp  -  LittleFS animation player
 * ============================================================================
 */
#include "anim.h"
#include <LittleFS.h>

void Animation::begin(uint8_t index) {
  setIndex(index);
}

void Animation::setIndex(uint8_t index) {
  idx_     = index % GIF_COUNT;
  frame_   = 0;
  started_ = false;
  nextMs_  = 0;
}

bool Animation::tick(uint16_t* dst) {
  const uint32_t now = millis();

  // Not due yet? (signed compare keeps it millis()-rollover safe)
  if (started_ && (int32_t)(now - nextMs_) < 0) return false;

  if (started_) {
    frame_++;
    if (frame_ >= frameCount()) frame_ = 0;
  }
  started_ = true;

  uint16_t fps = preset().fps;
  if (fps == 0) fps = 20;
  nextMs_ = now + (1000u / fps);

  return load(dst);
}

bool Animation::load(uint16_t* dst) {
  char name[40];
  snprintf(name, sizeof(name), "/%s_%u.raw", preset().prefix, (unsigned)frame_);

  File f = LittleFS.open(name, "r");
  if (!f) {
    // fall back to frame 0 so a partially uploaded animation still shows
    if (frame_ != 0) {
      frame_ = 0;
      snprintf(name, sizeof(name), "/%s_0.raw", preset().prefix);
      f = LittleFS.open(name, "r");
    }
    if (!f) return false;
  }

  const size_t want = (size_t)SCR_W * SCR_H * 2;
  const size_t got  = f.read((uint8_t*)dst, want);
  f.close();
  return got == want;
}
