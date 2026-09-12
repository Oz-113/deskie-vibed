/**
 * ============================================================================
 *  comms.cpp  -  UART (USB-CDC) line protocol, pinned to core 0
 * ============================================================================
 */
#include "comms.h"
#include <string.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
//  Enqueue API (thread safe, never blocks)
// ---------------------------------------------------------------------------
void commSend(OutType type, int16_t a, int16_t b) {
  OutMsg m = { type, a, b };
  xQueueSend(gOutQ, &m, 0);
}
void commVolStep(int8_t dir) { commSend(OUT_VOL_STEP, dir); }
void commVolSet(int pct)     { commSend(OUT_VOL_SET, pct); }
void commMute(bool on)       { commSend(OUT_MUTE_SET, on ? 1 : 0); }
void commMedia(MediaKey k)   { commSend(OUT_MEDIA, (int16_t)k); }
void commReqDetail(Comp c)   { commSend(OUT_REQ_DETAIL, (int16_t)c); }
void commPage(Page p)        { commSend(OUT_PAGE, (int16_t)p); }

// ---------------------------------------------------------------------------
//  Setup
// ---------------------------------------------------------------------------
void commInit(void) {
  Serial.begin(UART_BAUD);
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);                  // never block on USB CDC writes
#endif
  while (Serial.available()) Serial.read();  // drop any boot garbage
}

// ---------------------------------------------------------------------------
//  TX  (called only from taskComm)
// ---------------------------------------------------------------------------
static void emit(const OutMsg& m) {
  switch (m.type) {
    case OUT_HELLO:
      Serial.printf("H,%d\n", FW_PROTOCOL_VERSION);
      break;

    case OUT_VOL_STEP:
      Serial.printf("VOL,%+d\n", (int)m.a);
      break;

    case OUT_VOL_SET:
      Serial.printf("VSET,%d\n", (int)m.a);
      break;

    case OUT_MUTE_SET:
      Serial.printf("MUTE,%d\n", m.a ? 1 : 0);
      break;

    case OUT_MEDIA:
      Serial.printf("MK,%s\n", (m.a == MK_NEXT) ? "N" : (m.a == MK_PREV) ? "P" : "PP");
      break;

    case OUT_REQ_DETAIL: {
      char c = 'C';
      switch ((Comp)m.a) {
        case COMP_RAM:  c = 'R'; break;
        case COMP_GPU:  c = 'G'; break;
        case COMP_VRAM: c = 'V'; break;
        default:        c = 'C'; break;
      }
      Serial.printf("REQ,%c\n", c);
      break;
    }

    case OUT_PAGE: {
      const char* n = "MONITOR";
      switch ((Page)m.a) {
        case PAGE_VOLUME:  n = "VOLUME";  break;
        case PAGE_MEDIA:   n = "MEDIA";   break;
        case PAGE_GALLERY: n = "GALLERY"; break;
        default:           n = "MONITOR"; break;
      }
      Serial.printf("PG,%s\n", n);
      break;
    }

    default:
      break;
  }
}

// ---------------------------------------------------------------------------
//  RX helpers
// ---------------------------------------------------------------------------
// Parse up to 'max' comma separated integers from 's'. Returns how many were
// found. The string is modified in place.
static int parseInts(char* s, int16_t* out, int max) {
  int    n = 0;
  char*  p = s;
  while (p && *p && n < max) {
    char* comma = strchr(p, ',');
    if (comma) *comma = '\0';
    out[n++] = (int16_t)atoi(p);
    p = comma ? comma + 1 : nullptr;
  }
  return n;
}

static void handleLine(char* line) {
  // trim trailing CR / spaces
  size_t L = strlen(line);
  while (L && (line[L - 1] == '\r' || line[L - 1] == '\n' || line[L - 1] == ' ')) line[--L] = '\0';
  if (L == 0) return;

  stateMarkPcSeen();

  const char cmd  = line[0];
  char*      rest = strchr(line, ',');
  if (rest) { *rest = '\0'; rest++; } else { rest = line + L; }

  switch (cmd) {
    case 'H':                       // H,<protocol version>
      stateLock();
      gState.helloSeen = true;
      stateUnlock();
      break;

    case 'V': {                     // V,<volume 0..100>,<mute 0|1>
      int16_t v[2] = { -1, 0 };
      if (parseInts(rest, v, 2) >= 1) stateApplyVolume(v[0], v[1]);
      break;
    }

    case 'S': {                     // S,<cpu>,<ram>,<gpu>,<vram>
      int16_t v[4] = { -1, -1, -1, -1 };
      if (parseInts(rest, v, 4) >= 4) stateApplySummary(v[0], v[1], v[2], v[3]);
      break;
    }

    case 'E': {                     // E,<13 values, -1 == unavailable>
      int16_t v[DETAIL_FIELDS];
      memset(v, 0xFF, sizeof(v));   // every byte 0xFF -> each int16 == -1
      if (parseInts(rest, v, DETAIL_FIELDS) > 0) stateApplyDetail(v);
      break;
    }

    case 'T': {                     // T,<state>,<artist>|<title>
      int   st = atoi(rest);
      char* p  = strchr(rest, ',');
      char  buf[80];
      const char* ap = (p ? p + 1 : "");
      strncpy(buf, ap, sizeof(buf) - 1);
      buf[sizeof(buf) - 1] = '\0';
      char* bar = strchr(buf, '|');
      if (bar) { *bar = '\0'; stateApplyNowPlaying(st, buf, bar + 1); }
      else     { stateApplyNowPlaying(st, buf, ""); }
      break;
    }

    case 'A': {                     // A,<b0>,..,<b7>,<pulse>
      int16_t v[AUDIO_BANDS + 1];
      memset(v, 0, sizeof(v));
      if (parseInts(rest, v, AUDIO_BANDS + 1) >= AUDIO_BANDS) stateApplyAudio(v);
      break;
    }

    case 'P':                       // ping
      Serial.print("PONG\n");
      break;

    default:
      break;
  }
}

// ---------------------------------------------------------------------------
//  Task (core 0)
// ---------------------------------------------------------------------------
void taskComm(void* arg) {
  (void)arg;

  char     line[RX_LINE_MAX];
  size_t   len       = 0;
  uint32_t lastHello = 0;
  bool     wasUp     = false;

  const TickType_t period = pdMS_TO_TICKS(10);

  for (;;) {
    const uint32_t now = millis();

    // ---- RX: assemble lines ------------------------------------------
    while (Serial.available()) {
      const int c = Serial.read();
      if (c < 0) break;
      if (c == '\n') {
        line[len] = '\0';
        if (len) handleLine(line);
        len = 0;
      } else if (c != '\r') {
        if (len < RX_LINE_MAX - 1) line[len++] = (char)c;
        else                       len = 0;      // overflow: drop this line
      }
    }

    // ---- TX: drain the outgoing queue --------------------------------
    OutMsg m;
    while (xQueueReceive(gOutQ, &m, 0) == pdTRUE) emit(m);

    // ---- link bookkeeping --------------------------------------------
    // Use a FRESH timestamp: stateMarkPcSeen() runs during the RX phase above
    // and stamps lastPcRxMs with millis(), which can be one tick newer than the
    // 'now' captured at the top of the loop. The unsigned subtraction would
    // then underflow and the link would wrongly look dead.
    stateLock();
    const uint32_t nowMs = millis();
    const bool up    = (gState.lastPcRxMs != 0) &&
                       ((int32_t)(nowMs - gState.lastPcRxMs) < (int32_t)RX_STALE_MS);
    gState.pcUp      = up;
    const bool hello = gState.helloSeen;
    const Page page  = gState.page;
    stateUnlock();

    if (up && !wasUp) {                 // just (re)connected -> resync the PC
      commSend(OUT_HELLO);
      commSend(OUT_PAGE, (int16_t)page);
      commSend(OUT_REQ_DETAIL, (int16_t)COMP_CPU);
      lastHello = now;
    }
    wasUp = up;

    if (!hello && (now - lastHello) >= 2000) {   // keep announcing until acked
      lastHello = now;
      commSend(OUT_HELLO);
    }

    vTaskDelay(period);
  }
}

