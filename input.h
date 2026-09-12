/**
 * ============================================================================
 *  input.h  -  Rotary encoder + buttons, pinned to core 0
 * ============================================================================
 *  The encoder uses a quadrature state machine in an ISR (as in the original
 *  project). Buttons are debounced here and turned into InputEvent items on
 *  gInputQ. Long/short detection of the encoder button is handled here too.
 * ============================================================================
 */
#pragma once

#include <Arduino.h>
#include "state.h"

void inputInit(void);
void taskInput(void* arg);
