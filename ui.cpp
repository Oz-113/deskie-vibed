/**
 * ============================================================================
 *  ui.cpp  -  Screens, navigation state machine and rendering (core 1)
 * ============================================================================
 */
#include "ui.h"
#include "display.h"
#include "comms.h"
#include <math.h>
#include <string.h>

// ===========================================================================
//  UI-owned state (only ever touched from the render task)
// ===========================================================================
namespace {

Animation* s_anim = nullptr;

Mode    s_mode       = MODE_IDLE;
Page    s_menuPage   = PAGE_MONITOR;   // highlighted tile
Page    s_page       = PAGE_MONITOR;   // opened page
Comp    s_selComp    = COMP_CPU;
uint8_t s_selMedia   = 0;              // 0 play/pause, 1 next, 2 prev
uint8_t s_galleryCur = 0;

uint32_t s_lastUser      = 0;
bool     s_dirty         = true;
uint32_t s_lastDetailReq = 0;

uint32_t s_prevSig = 0;
bool     s_haveSig = false;

const Mode PAGE_MODE[PAGE_COUNT] = { MODE_MONITOR, MODE_VOLUME, MODE_MEDIA, MODE_GALLERY };

// ---------------------------------------------------------------------------
//  Small helpers
// ---------------------------------------------------------------------------
int wrapI(int v, int n) { v %= n; if (v < 0) v += n; return v; }

// Trim 's' in place until it fits 'maxW' pixels in 'font'.
void fitText(char* s, int maxW, uint8_t font) {
  if (spr.textWidth(s, font) <= maxW) return;
  size_t n = strlen(s);
  while (n > 1 && spr.textWidth(s, font) > maxW) s[--n] = '\0';
}

uint32_t sigMix(uint32_t h, int32_t v) { return (h ^ (uint32_t)v) * 16777619u; }

uint32_t makeSig(const Runtime& st, uint16_t animFrame) {
  uint32_t h = 2166136261u;
  h = sigMix(h, (int32_t)s_mode);
  h = sigMix(h, (int32_t)s_menuPage);
  h = sigMix(h, (int32_t)s_page);
  h = sigMix(h, (int32_t)s_selComp);
  h = sigMix(h, (int32_t)s_selMedia);
  h = sigMix(h, (int32_t)s_galleryCur);
  h = sigMix(h, (int32_t)st.volume);
  h = sigMix(h, st.mute ? 1 : 0);
  h = sigMix(h, st.pcUp ? 1 : 0);
  h = sigMix(h, (int32_t)st.np.state);
  h = sigMix(h, (int32_t)animFrame);

  const Metrics& m = st.met;
  h = sigMix(h, m.cpu);         h = sigMix(h, m.ram);
  h = sigMix(h, m.gpu);         h = sigMix(h, m.vram);
  h = sigMix(h, m.cpuTempX10);  h = sigMix(h, m.cpuPowerX10);
  h = sigMix(h, m.cpuFreqMhz);  h = sigMix(h, m.cpuCores);
  h = sigMix(h, m.ramUsedMb);   h = sigMix(h, m.ramTotalMb);
  h = sigMix(h, m.gpuTempX10);  h = sigMix(h, m.gpuHotX10);
  h = sigMix(h, m.gpuPowerX10); h = sigMix(h, m.gpuFan);
  h = sigMix(h, m.vramUsedMb);  h = sigMix(h, m.vramTotalMb);

  for (const char* p = st.np.title;  *p; ++p) h = sigMix(h, (uint8_t)*p);
  for (const char* p = st.np.artist; *p; ++p) h = sigMix(h, (uint8_t)*p);
  return h;
}

void touch(void) { s_lastUser = millis(); s_dirty = true; }

// ---------------------------------------------------------------------------
//  Navigation
// ---------------------------------------------------------------------------
void enterPage(Page p) {
  s_page = p;
  s_mode = PAGE_MODE[p];
  s_lastDetailReq = 0;

  stateLock();
  gState.page = p;
  stateUnlock();

  commPage(p);
  touch();
}

void goMenu(void) {
  s_mode     = MODE_MENU;
  s_menuPage = s_page;
  touch();
}

void onEncoder(int dir) {
  switch (s_mode) {
    case MODE_IDLE:                       // first turn opens the menu
      s_mode     = MODE_MENU;
      s_menuPage = (Page)wrapI((int)s_page + dir, PAGE_COUNT);
      break;

    case MODE_MENU:
      s_menuPage = (Page)wrapI((int)s_menuPage + dir, PAGE_COUNT);
      break;

    case MODE_MONITOR:
      s_selComp = (Comp)wrapI((int)s_selComp + dir, COMP_COUNT);
      break;

    case MODE_DETAIL:
      s_selComp = (Comp)wrapI((int)s_selComp + dir, COMP_COUNT);
      s_lastDetailReq = 0;                // fetch the new component's detail
      break;

    case MODE_VOLUME: {
      stateLock();                        // optimistic local update first
      if (gState.volume < 0) gState.volume = 0;
      gState.volume = (int16_t)constrain((int)gState.volume + dir, 0, 100);
      gState.mute   = false;
      const int16_t v = gState.volume;
      stateUnlock();
      // Send the ABSOLUTE target, never a relative step: on the PC side
      // GetMasterVolumeLevelScalar() lags slightly behind a write, so a
      // read-modify-write there drifts or even moves the wrong way.
      commVolSet(v);
      break;
    }

    case MODE_MEDIA:
      s_selMedia = (uint8_t)wrapI((int)s_selMedia + dir, 3);
      break;

    case MODE_GALLERY:
      s_galleryCur = (uint8_t)wrapI((int)s_galleryCur + dir, GIF_COUNT);
      if (s_anim) s_anim->setIndex(s_galleryCur);   // live preview
      break;

    default: break;
  }
  touch();
}

void onShort(void) {
  switch (s_mode) {
    case MODE_IDLE:
      goMenu();
      return;

    case MODE_MENU:
      enterPage(s_menuPage);
      return;

    case MODE_MONITOR:
      s_mode = MODE_DETAIL;
      s_lastDetailReq = 0;
      break;

    case MODE_DETAIL:
      s_lastDetailReq = 0;                // refresh on demand
      break;

    case MODE_VOLUME: {
      bool on;
      stateLock();
      on = !gState.mute;
      gState.mute = on;
      stateUnlock();
      commMute(on);
      break;
    }

    case MODE_MEDIA: {
      const MediaKey k = (s_selMedia == 0) ? MK_PLAYPAUSE
                       : (s_selMedia == 1) ? MK_NEXT : MK_PREV;
      commMedia(k);
      break;
    }

    case MODE_GALLERY:
      stateLock();
      gState.gifIndex = s_galleryCur;     // commit the choice
      stateUnlock();
      if (s_anim) s_anim->setIndex(s_galleryCur);
      goMenu();
      return;

    default: break;
  }
  touch();
}

void onLong(void) {
  switch (s_mode) {
    case MODE_IDLE:
      break;

    case MODE_DETAIL:
      s_mode = MODE_MONITOR;
      break;

    case MODE_MENU:
      s_mode = MODE_IDLE;                 // the IDLE render re-syncs the gif
      break;

    case MODE_MONITOR:
    case MODE_VOLUME:
    case MODE_MEDIA:
      goMenu();
      return;

    case MODE_GALLERY: {
      uint8_t committed;
      stateLock();
      committed = gState.gifIndex;        // discard the preview
      stateUnlock();
      if (s_anim) s_anim->setIndex(committed);
      goMenu();
      return;
    }

    default: break;
  }
  touch();
}

}  // namespace

// ===========================================================================
//  Public API
// ===========================================================================
void uiInit(Animation* anim) {
  s_anim = anim;

  stateLock();
  s_page = gState.page;
  s_galleryCur = gState.gifIndex;
  stateUnlock();

  s_mode     = MODE_IDLE;
  s_menuPage = s_page;
  s_lastUser = millis();
  s_dirty    = true;
  s_haveSig  = false;
}

void uiHandleEvent(const InputEvent& e) {
  switch (e.type) {
    case EV_ENC_CW:    onEncoder(+1);  break;
    case EV_ENC_CCW:   onEncoder(-1);  break;
    case EV_BTN_SHORT: onShort();      break;
    case EV_BTN_LONG:  onLong();       break;

    // dedicated media buttons work from any screen
    case EV_MKEY_PLAY: commMedia(MK_PLAYPAUSE); touch(); break;
    case EV_MKEY_NEXT: commMedia(MK_NEXT);      touch(); break;
    case EV_MKEY_PREV: commMedia(MK_PREV);      touch(); break;

    // reserved shortcut buttons
    case EV_SC_MONITOR: enterPage(PAGE_MONITOR); break;
    case EV_SC_VOLUME:  enterPage(PAGE_VOLUME);  break;
    case EV_SC_MEDIA:   enterPage(PAGE_MEDIA);   break;
    case EV_SC_GALLERY: enterPage(PAGE_GALLERY); break;

    default: break;
  }
}

// ===========================================================================
//  Rendering
// ===========================================================================
namespace {

// ---- number formatting helpers (no dynamic allocation) --------------------
const char* fmtPct(int pct, char* b, size_t n) {
  if (pct < 0) snprintf(b, n, "--"); else snprintf(b, n, "%d", pct);
  return b;
}
// as above but with the "%" sign, for the big centre readings
const char* fmtPctSign(int pct, char* b, size_t n) {
  if (pct < 0) snprintf(b, n, "--"); else snprintf(b, n, "%d%%", pct);
  return b;
}
const char* fmtTemp(int x10, char* b, size_t n) {
  if (x10 < 0) snprintf(b, n, "--"); else snprintf(b, n, "%.1fC", x10 / 10.0f);
  return b;
}
const char* fmtPower(int x10, char* b, size_t n) {
  if (x10 < 0) snprintf(b, n, "--"); else snprintf(b, n, "%.1fW", x10 / 10.0f);
  return b;
}
const char* fmtGb(int mb, char* b, size_t n) {
  if (mb < 0) snprintf(b, n, "--"); else snprintf(b, n, "%.1fG", mb / 1024.0f);
  return b;
}
const char* fmtMhz(int mhz, char* b, size_t n) {
  if (mhz < 0)                 snprintf(b, n, "--");
  else if (mhz >= 1000)        snprintf(b, n, "%.2fG", mhz / 1000.0f);
  else                         snprintf(b, n, "%dM", mhz);
  return b;
}

// centred text with a 1px black drop shadow so it stays readable over art
void textShadow(const char* t, int x, int y, uint8_t font, uint16_t col) {
  dispTextCentre(t, x + 1, y + 1, font, TFT_BLACK);
  dispTextCentre(t, x, y, font, col);
}

// ---- forward declarations --------------------------------------------------
void drawIdle(const Runtime& st, uint16_t accent, uint16_t hi);
void drawMenu(uint16_t accent, uint16_t hi, uint16_t tint);
void drawMonitor(const Runtime& st, uint16_t accent, uint16_t hi, uint16_t tint);
void drawDetail(const Runtime& st, uint16_t accent, uint16_t hi, uint16_t tint);
void drawVolume(const Runtime& st, uint16_t accent, uint16_t hi, uint16_t tint);
void drawMedia(const Runtime& st, uint16_t accent, uint16_t hi, uint16_t tint);
void drawGallery(uint16_t accent, uint16_t hi, uint16_t tint);

// ---------------------------------------------------------------------------
//  IDLE : the animation plus an optional now-playing ticker
// ---------------------------------------------------------------------------
void drawIdle(const Runtime& st, uint16_t accent, uint16_t hi) {
  (void)accent;
  if (st.np.state == 0 && st.np.title[0] == '\0') return;

  char title[42], artist[34];
  strncpy(title, st.np.title, sizeof(title) - 1);   title[sizeof(title) - 1] = '\0';
  strncpy(artist, st.np.artist, sizeof(artist) - 1); artist[sizeof(artist) - 1] = '\0';

  // Rotating vector ornament. Its angle comes from wall-clock time, so the
  // spin speed is independent of the animation's frame rate.
  const int spin = (int)((millis() % IDLE_SPIN_PERIOD_MS) * 360UL / IDLE_SPIN_PERIOD_MS);
  dispSpinner(120, 220, 12, 8, spin, hi);
  spr.fillSmoothCircle(120, 220, 3, hi, 0x0000);        // hub

  // Now playing, drawn in the palette's highlight colour so the text and the
  // spinning ornament read as one piece.
  fitText(title, 130, 2);
  if (title[0]) textShadow(title, 133, 197, 2, hi);

  fitText(artist, 122, 1);
  if (artist[0]) textShadow(artist, 133, 213, 1, hi);
}

// ---------------------------------------------------------------------------
//  MENU : 2x2 page tiles
// ---------------------------------------------------------------------------
void drawMenu(uint16_t accent, uint16_t hi, uint16_t tint) {
  const int gap = 12;
  const int total = MENU_TILE_W * 2 + gap;
  const int x0 = SCR_CX - total / 2;
  const int y0 = SCR_CY - total / 2;

  dispTextCentre("MENU", SCR_CX, 30, 2, accent);

  for (uint8_t i = 0; i < PAGE_COUNT; i++) {
    const int  x   = x0 + (i % 2) * (MENU_TILE_W + gap);
    const int  y   = y0 + (i / 2) * (MENU_TILE_H + gap);
    const bool sel = ((Page)i == s_menuPage);

    spr.fillSmoothRoundRect(x, y, MENU_TILE_W, MENU_TILE_H, MENU_TILE_R, sel ? tint : 0x1082);
    spr.drawRoundRect(x, y, MENU_TILE_W, MENU_TILE_H, MENU_TILE_R, sel ? hi : accent);
    dispPageIcon((Page)i, x + MENU_TILE_W / 2, y + MENU_TILE_H / 2 - 7, 34, sel ? hi : accent);
    dispTextCentre(pageName((Page)i), x + MENU_TILE_W / 2, y + MENU_TILE_H - 10, 1,
                   sel ? hi : accent);
  }

  dispTextCentre("TURN=MOVE  OK=OPEN", SCR_CX, y0 + total + 16, 1, accent);
}

// ---------------------------------------------------------------------------
//  MONITOR : four resource gauges + a centre readout
// ---------------------------------------------------------------------------
int metPct(const Metrics& m, Comp c) {
  switch (c) {
    case COMP_CPU:  return m.cpu;
    case COMP_RAM:  return m.ram;
    case COMP_GPU:  return m.gpu;
    case COMP_VRAM: return m.vram;
    default:        return -1;
  }
}

void drawMonitor(const Runtime& st, uint16_t accent, uint16_t hi, uint16_t tint) {
  static const int ANG[COMP_COUNT] = { MON_ANG_CPU, MON_ANG_RAM, MON_ANG_GPU, MON_ANG_VRAM };
  char b[8];

  for (uint8_t i = 0; i < COMP_COUNT; i++) {
    const Comp c   = (Comp)i;
    const bool sel = (c == s_selComp);
    const int  pct = metPct(st.met, c);

    dispGauge(SCR_CX, SCR_CY, MON_R_OUTER, MON_R_INNER, ANG[i], MON_SWEEP_DEG, pct,
              sel ? hi : accent, tint, sel);

    dispRadialText(SCR_CX, SCR_CY, MON_LABEL_R, ANG[i], compName(c), sel ? hi : accent, 1);
    dispRadialText(SCR_CX, SCR_CY, MON_TEXT_R,  ANG[i], fmtPct(pct, b, sizeof(b)),
                   sel ? hi : accent, 4);
  }

  // centre readout of the highlighted component
  dispTextCentre(compName(s_selComp), SCR_CX, SCR_CY - 34, 2, accent);
  dispTextCentre(fmtPctSign(metPct(st.met, s_selComp), b, sizeof(b)), SCR_CX, SCR_CY + 6, 6, hi);

  if (!st.pcUp) dispTextCentre("LINK DOWN", SCR_CX, SCR_CY + 42, 1, 0xF800);
}

// ---------------------------------------------------------------------------
//  DETAIL : one component full screen (big gauge + numbers)
// ---------------------------------------------------------------------------
void drawDetail(const Runtime& st, uint16_t accent, uint16_t hi, uint16_t tint) {
  const Comp     c     = s_selComp;
  const Metrics& m     = st.met;
  const int      pct   = metPct(m, c);
  const int      sweep = BIG_END_DEG - BIG_START_DEG;
  char b[8];

  dispGauge(SCR_CX, SCR_CY, BIG_R_OUTER, BIG_R_INNER,
            BIG_START_DEG + sweep / 2, sweep, pct, hi, tint, true);

  dispTextCentre(compName(c), SCR_CX, 34, 2, accent);
  dispTextCentre(fmtPctSign(pct, b, sizeof(b)), SCR_CX, SCR_CY, 7, hi);

  char r[4][24];
  int  rn = 0;
  for (int i = 0; i < 4; i++) r[i][0] = '\0';

  switch (c) {
    case COMP_CPU:
      snprintf(r[rn++], 24, "TEMP  %s",  fmtTemp(m.cpuTempX10,  b, sizeof(b)));
      snprintf(r[rn++], 24, "POWER %s",  fmtPower(m.cpuPowerX10, b, sizeof(b)));
      snprintf(r[rn++], 24, "CLOCK %s",  fmtMhz(m.cpuFreqMhz,    b, sizeof(b)));
      if (m.cpuCores >= 0) snprintf(r[rn++], 24, "THREADS %d", m.cpuCores);
      break;
    case COMP_RAM:
      snprintf(r[rn++], 24, "USED  %s",  fmtGb(m.ramUsedMb,  b, sizeof(b)));
      snprintf(r[rn++], 24, "TOTAL %s",  fmtGb(m.ramTotalMb, b, sizeof(b)));
      break;
    case COMP_GPU:
      snprintf(r[rn++], 24, "TEMP  %s",  fmtTemp(m.gpuTempX10,  b, sizeof(b)));
      snprintf(r[rn++], 24, "HOT   %s",  fmtTemp(m.gpuHotX10,   b, sizeof(b)));
      snprintf(r[rn++], 24, "POWER %s",  fmtPower(m.gpuPowerX10, b, sizeof(b)));
      if (m.gpuFan >= 0) snprintf(r[rn++], 24, "FAN   %d%%", m.gpuFan);
      break;
    case COMP_VRAM:
      snprintf(r[rn++], 24, "USED  %s",  fmtGb(m.vramUsedMb,  b, sizeof(b)));
      snprintf(r[rn++], 24, "TOTAL %s",  fmtGb(m.vramTotalMb, b, sizeof(b)));
      break;
    default: break;
  }

  static const int RY[4] = { 162, 176, 190, 204 };
  static const int RW[4] = { 170, 150, 126,  94 };
  for (int i = 0; i < rn; i++) {
    fitText(r[i], RW[i], 1);
    dispTextCentre(r[i], SCR_CX, RY[i], 1, accent);
  }

  dispTextCentre("HOLD=BACK", SCR_CX, 220, 1, accent);
}

// ---------------------------------------------------------------------------
//  VOLUME : big gauge, encoder adjusts, press mutes
// ---------------------------------------------------------------------------
void drawVolume(const Runtime& st, uint16_t accent, uint16_t hi, uint16_t tint) {
  const bool known = (st.volume >= 0);
  const int  pct   = known ? st.volume : -1;
  const int  sweep = BIG_END_DEG - BIG_START_DEG;
  const bool muted = st.mute;

  dispGauge(SCR_CX, SCR_CY, BIG_R_OUTER, BIG_R_INNER,
            BIG_START_DEG + sweep / 2, sweep, pct,
            muted ? (uint16_t)(hi >> 2) : hi, tint, true);

  dispTextCentre(muted ? "MUTED" : "VOLUME", SCR_CX, 34, 2, muted ? (uint16_t)0xF800 : accent);

  if (known) {
    char b[8];
    snprintf(b, sizeof(b), "%d", st.volume);
    dispTextCentre(b, SCR_CX, SCR_CY - 6, 7, muted ? (uint16_t)0x7BEF : hi);
    dispTextCentre("%", SCR_CX, SCR_CY + 38, 2, accent);
  } else {
    dispTextCentre("--", SCR_CX, SCR_CY, 7, (uint16_t)0x7BEF);
  }

  dispTextCentre("TURN=VOL  OK=MUTE", SCR_CX, 220, 1, accent);
}

// ---------------------------------------------------------------------------
//  MEDIA : transport controls + now playing
// ---------------------------------------------------------------------------
void drawMedia(const Runtime& st, uint16_t accent, uint16_t hi, uint16_t tint) {
  (void)tint;

  char title[42], artist[34];
  strncpy(title,  st.np.title,  sizeof(title) - 1);  title[sizeof(title) - 1]   = '\0';
  strncpy(artist, st.np.artist, sizeof(artist) - 1); artist[sizeof(artist) - 1] = '\0';
  fitText(title,  150, 2);
  fitText(artist, 140, 1);

  dispTextCentre(title[0] ? title : "NO MEDIA", SCR_CX, 40, 2, accent);
  dispTextCentre(artist, SCR_CX, 58, 1, accent);

  const int  cy      = SCR_CY + 8;
  const bool selPlay = (s_selMedia == 0);
  const bool selNext = (s_selMedia == 1);
  const bool selPrev = (s_selMedia == 2);

  // previous / next chevrons
  spr.fillTriangle(48, cy - 16, 48, cy + 16, 76, cy, selPrev ? hi : accent);
  spr.fillTriangle(192, cy - 16, 192, cy + 16, 164, cy, selNext ? hi : accent);

  // centre play / pause
  if (selPlay) spr.drawCircle(SCR_CX, cy, 34, hi);
  if (st.np.state == 1) {                       // playing -> pause bars
    spr.fillRect(SCR_CX - 11, cy - 15, 8, 30, selPlay ? hi : accent);
    spr.fillRect(SCR_CX +  3, cy - 15, 8, 30, selPlay ? hi : accent);
  } else {                                      // paused -> play triangle
    spr.fillTriangle(SCR_CX - 11, cy - 16, SCR_CX - 11, cy + 16, SCR_CX + 14, cy,
                     selPlay ? hi : accent);
  }

  dispTextCentre("TURN=PICK  OK=GO", SCR_CX, 220, 1, accent);
}

// ---------------------------------------------------------------------------
//  GALLERY : segmented ring animation picker
// ---------------------------------------------------------------------------
void drawGallery(uint16_t accent, uint16_t hi, uint16_t tint) {
  const int seg = 360 / GIF_COUNT;

  for (uint8_t i = 0; i < GIF_COUNT; i++) {
    const bool sel = (i == s_galleryCur);
    dispGauge(SCR_CX, SCR_CY, GAL_R_OUTER, GAL_R_INNER,
              i * seg + seg / 2, seg - GAL_SEG_GAP_DEG, 100,
              sel ? hi : accent, tint, sel);
  }

  dispTextCentre("GALLERY", SCR_CX, 30, 2, accent);

  // The ring re-divides itself from GIF_COUNT, so a new animation needs no UI
  // work at all - just make sure a long prefix still fits the centre.
  char name[28];
  strncpy(name, GIF_TABLE[s_galleryCur].prefix, sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  fitText(name, 150, 2);
  dispTextCentre(name, SCR_CX, SCR_CY - 8, 2, hi);

  char b[16];
  snprintf(b, sizeof(b), "%u / %u", (unsigned)(s_galleryCur + 1), (unsigned)GIF_COUNT);
  dispTextCentre(b, SCR_CX, SCR_CY + 12, 1, accent);

  dispTextCentre("OK=SELECT HOLD=EXIT", SCR_CX, 214, 1, accent);
}

// ---------------------------------------------------------------------------
//  Timeouts / periodic detail fetch
// ---------------------------------------------------------------------------
void handleTimeout(uint32_t now) {
  // The resource monitor is meant to be left on screen, so it never times out.
  if (s_mode == MODE_IDLE || s_mode == MODE_MONITOR) return;

  if (s_mode == MODE_DETAIL) {
    // a detail page is a sub-page of the monitor, so it falls back there
    if ((now - s_lastUser) > UI_DETAIL_TIMEOUT_MS) {
      s_mode  = MODE_MONITOR;
      s_dirty = true;
      return;
    }
    if ((now - s_lastDetailReq) >= DETAIL_REQ_INTERVAL_MS) {
      s_lastDetailReq = now;
      commReqDetail(s_selComp);
    }
    return;
  }

  // MENU / VOLUME / MEDIA / GALLERY fall back to the animation
  if ((now - s_lastUser) > UI_IDLE_TIMEOUT_MS) {
    s_mode  = MODE_IDLE;
    s_dirty = true;
  }
}

}  // namespace

// ===========================================================================
//  Render entry point
// ===========================================================================
bool uiRender(Animation& anim, bool frameLoaded) {
  // 1. Apply everything the input task produced. Pumping the queue here (rather
  //    than in the task) guarantees the UI can never miss an event, whoever
  //    calls uiRender().
  InputEvent ev;
  while (xQueueReceive(gInputQ, &ev, 0) == pdTRUE) {
    uiHandleEvent(ev);
  }

  // 2. Timeouts / periodic detail requests
  const uint32_t now = millis();
  handleTimeout(now);

  // Snapshot the shared state once, then work on the copy (no lock held).
  Runtime st;
  stateLock();
  st = gState;
  stateUnlock();

  // IDLE always plays the committed animation
  if (s_mode == MODE_IDLE && anim.index() != st.gifIndex) anim.setIndex(st.gifIndex);

  const uint32_t sig = makeSig(st, anim.frameIndex());
  // A freshly loaded frame can carry the same frame index as the previous one
  // (1-frame animations, or switching gif onto the same frame number), so the
  // frameLoaded flag has to force the repaint.
  if (!s_dirty && !frameLoaded && s_haveSig && sig == s_prevSig) return false;
  s_prevSig = sig;
  s_haveSig = true;
  s_dirty   = false;

  const uint16_t accent = anim.accent();
  const uint16_t hi     = anim.highlight();
  const uint16_t tint   = anim.tint();

  if (s_mode == MODE_IDLE) {
    spr.pushImage(0, 0, SCR_W, SCR_H, gFrameBuf);
    drawIdle(st, accent, hi);
  } else {
    dispDimCopy(1);                        // dimmed glass background
    spr.pushImage(0, 0, SCR_W, SCR_H, gDimBuf);
    switch (s_mode) {
      case MODE_MENU:    drawMenu(accent, hi, tint);        break;
      case MODE_MONITOR: drawMonitor(st, accent, hi, tint); break;
      case MODE_DETAIL:  drawDetail(st, accent, hi, tint);  break;
      case MODE_VOLUME:  drawVolume(st, accent, hi, tint);  break;
      case MODE_MEDIA:   drawMedia(st, accent, hi, tint);   break;
      case MODE_GALLERY: drawGallery(accent, hi, tint);     break;
      default: break;
    }
  }
  return true;
}


