/**
 * ============================================================================
 *  ui.h  -  Screens, navigation state machine and rendering
 * ============================================================================
 *  Screens
 *    IDLE     : the animation, plus an optional now-playing ticker
 *    MENU     : page selector (the original project's tile menu)
 *    MONITOR  : four resource gauges (CPU / RAM / GPU / VRAM)
 *    DETAIL   : one component "full screen" with extra numbers
 *    VOLUME   : volume gauge (encoder adjusts, press mutes)
 *    MEDIA    : transport + now playing
 *    GALLERY  : animation picker (segmented ring)
 *
 *  Navigation
 *    turn encoder ....... move selection / change value
 *    short press ........ OK / enter
 *    long  press ........ back one level
 *    idle timeout ....... return to IDLE
 * ============================================================================
 */
#pragma once

#include <Arduino.h>
#include "state.h"
#include "anim.h"

void uiInit(Animation* anim);
void uiHandleEvent(const InputEvent& e);

// Redraw for the current state. Returns true when the caller should push the
// sprite to the panel (i.e. something actually changed).
// 'frameLoaded' must be true whenever Animation::tick() loaded a new frame, so
// a new frame is always painted even if no numeric value changed.
bool uiRender(Animation& anim, bool frameLoaded);
