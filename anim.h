/**
 * ============================================================================
 *  anim.h  -  LittleFS animation player
 * ============================================================================
 *  Owned by the render task, so LittleFS is only ever touched from core 1.
 * ============================================================================
 */
#pragma once

#include <Arduino.h>
#include "state.h"

class Animation {
 public:
  void    begin(uint8_t index);
  void    setIndex(uint8_t index);              // switch animation (resets frame)
  uint8_t index(void) const { return idx_; }

  // Advance timing; when the next frame is due it is read into 'dst'.
  // Returns true if a frame was successfully loaded.
  bool tick(uint16_t* dst);

  const GifPreset& preset(void) const { return GIF_TABLE[idx_]; }
  uint16_t accent(void)    const { return preset().accent; }
  uint16_t highlight(void) const { return preset().highlight; }
  uint16_t tint(void)      const { return preset().tint; }
  uint16_t frameIndex(void) const { return frame_; }
  uint16_t frameCount(void) const { uint16_t f = preset().frames; return f ? f : 1; }

 private:
  bool load(uint16_t* dst);

  uint8_t  idx_     = 0;
  uint16_t frame_   = 0;
  bool     started_ = false;
  uint32_t nextMs_  = 0;
};
