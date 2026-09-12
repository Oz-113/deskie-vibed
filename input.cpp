/**
 * ============================================================================
 *  input.cpp  -  Rotary encoder + buttons, pinned to core 0
 * ============================================================================
 */
#include "input.h"

// ===========================================================================
//  Encoder: quadrature state machine in an ISR (as in the original project)
// ===========================================================================
static volatile int32_t gEncoderSteps = 0;

static const int8_t ENC_STATES[16] = { 0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0 };

static void IRAM_ATTR readEncoder(void) {
  static uint8_t old_AB = 3;
  static int8_t  internalState = 0;

  old_AB <<= 2;
  old_AB |= (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);
  internalState += ENC_STATES[old_AB & 0x0F];

  if (internalState > 3)       { gEncoderSteps++; internalState = 0; }
  else if (internalState < -3) { gEncoderSteps--; internalState = 0; }
}

// ===========================================================================
//  Buttons
// ===========================================================================
struct BtnState {
  bool     stable     = false;
  bool     rawLast    = false;
  uint32_t chgMs      = 0;
  uint32_t lastFireMs = 0;
};

// Debounced edge detector. Returns true on a *debounced* state change.
static bool btnUpdate(BtnState& b, uint8_t pin, uint32_t now) {
  const bool raw = (digitalRead(pin) == LOW);   // active LOW
  if (raw != b.rawLast) { b.rawLast = raw; b.chgMs = now; }
  if ((now - b.chgMs) >= BTN_DEBOUNCE_MS && b.stable != raw) {
    b.stable = raw;
    return true;
  }
  return false;
}

static void post(EvType t) {
  InputEvent e = { t };
  xQueueSend(gInputQ, &e, 0);   // never block; dropping while overloaded is fine
}

// ===========================================================================
//  Setup
// ===========================================================================
void inputInit(void) {
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), readEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), readEncoder, CHANGE);

  pinMode(PIN_ENC_BTN,  INPUT_PULLUP);
  pinMode(PIN_BTN_PLAY, INPUT_PULLUP);
  pinMode(PIN_BTN_NEXT, INPUT_PULLUP);
  pinMode(PIN_BTN_PREV, INPUT_PULLUP);

#if ENABLE_SHORTCUT_BUTTONS
  pinMode(PIN_SC_MONITOR, INPUT_PULLUP);
  pinMode(PIN_SC_VOLUME,  INPUT_PULLUP);
  pinMode(PIN_SC_MEDIA,   INPUT_PULLUP);
  pinMode(PIN_SC_GALLERY, INPUT_PULLUP);
#endif
}

// ===========================================================================
//  Task (core 0)
// ===========================================================================
void taskInput(void* arg) {
  (void)arg;

  // encoder push button: needs short/long discrimination
  BtnState eb;
  uint32_t ebDownMs = 0;
  bool     ebLong   = false;

  // dedicated media buttons
  BtnState bPlay, bNext, bPrev;

#if ENABLE_SHORTCUT_BUTTONS
  BtnState bScMon, bScVol, bScMed, bScGal;
#endif

  const TickType_t period = pdMS_TO_TICKS(2);

  for (;;) {
    const uint32_t now = millis();

    // ---- rotary encoder ------------------------------------------------
    int32_t steps;
    noInterrupts();
    steps = gEncoderSteps;
    gEncoderSteps = 0;
    interrupts();

    if (steps >  8) steps =  8;   // cap bursts
    if (steps < -8) steps = -8;
    while (steps > 0) { post(EV_ENC_CW);  steps--; }
    while (steps < 0) { post(EV_ENC_CCW); steps++; }

    // ---- encoder push button: short = OK, long = back -------------------
    if (btnUpdate(eb, PIN_ENC_BTN, now)) {
      if (eb.stable) { ebDownMs = now; ebLong = false; }
      else if (!ebLong) { post(EV_BTN_SHORT); }
    }
    if (eb.stable && !ebLong && (now - ebDownMs) >= LONGPRESS_MS) {
      ebLong = true;
      post(EV_BTN_LONG);
    }

    // ---- dedicated media buttons ---------------------------------------
    if (btnUpdate(bPlay, PIN_BTN_PLAY, now) && bPlay.stable &&
        (now - bPlay.lastFireMs) >= BTN_MEDIA_REPEAT_MS) {
      bPlay.lastFireMs = now; post(EV_MKEY_PLAY);
    }
    if (btnUpdate(bNext, PIN_BTN_NEXT, now) && bNext.stable &&
        (now - bNext.lastFireMs) >= BTN_MEDIA_REPEAT_MS) {
      bNext.lastFireMs = now; post(EV_MKEY_NEXT);
    }
    if (btnUpdate(bPrev, PIN_BTN_PREV, now) && bPrev.stable &&
        (now - bPrev.lastFireMs) >= BTN_MEDIA_REPEAT_MS) {
      bPrev.lastFireMs = now; post(EV_MKEY_PREV);
    }

#if ENABLE_SHORTCUT_BUTTONS
    // Update all four first (each needs its own debounce state), then act on edges
    const bool eMon = btnUpdate(bScMon, PIN_SC_MONITOR, now);
    const bool eVol = btnUpdate(bScVol, PIN_SC_VOLUME,  now);
    const bool eMed = btnUpdate(bScMed, PIN_SC_MEDIA,   now);
    const bool eGal = btnUpdate(bScGal, PIN_SC_GALLERY, now);

    if (eMon && bScMon.stable) post(EV_SC_MONITOR);
    if (eVol && bScVol.stable) post(EV_SC_VOLUME);
    if (eMed && bScMed.stable) post(EV_SC_MEDIA);
    if (eGal && bScGal.stable) post(EV_SC_GALLERY);
#endif

    vTaskDelay(period);
  }
}
