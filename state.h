/**
 * ============================================================================
 *  state.h  -  Shared runtime state + inter-task communication primitives
 * ============================================================================
 *  Threading contract:
 *    - gState is ONLY touched while holding gStateMtx (use stateLock()/stateUnlock())
 *    - taskInput  -> gInputQ  (InputEvent)      -> consumed by the renderer
 *    - renderer   -> gOutQ    (OutMsg)          -> consumed by taskComm (UART TX)
 * ============================================================================
 */
#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "config.h"

// ---------------------------------------------------------------------------
//  Enumerations
// ---------------------------------------------------------------------------
enum Page : uint8_t { PAGE_MONITOR = 0, PAGE_VOLUME, PAGE_MEDIA, PAGE_GALLERY, PAGE_COUNT };
enum Mode : uint8_t { MODE_IDLE = 0, MODE_MENU, MODE_MONITOR, MODE_DETAIL, MODE_VOLUME, MODE_MEDIA, MODE_GALLERY };
enum Comp : uint8_t { COMP_CPU = 0, COMP_RAM, COMP_GPU, COMP_VRAM, COMP_COUNT };

// Input events: produced by taskInput, consumed by the renderer/UI logic
enum EvType : uint8_t {
  EV_ENC_CW = 0, EV_ENC_CCW, EV_BTN_SHORT, EV_BTN_LONG,
  EV_MKEY_PLAY, EV_MKEY_NEXT, EV_MKEY_PREV,
  EV_SC_MONITOR, EV_SC_VOLUME, EV_SC_MEDIA, EV_SC_GALLERY
};
struct InputEvent { EvType type; };

// Outgoing messages: produced by the renderer, consumed by taskComm
enum OutType : uint8_t {
  OUT_HELLO = 0, OUT_VOL_STEP, OUT_VOL_SET, OUT_MUTE_SET, OUT_MEDIA, OUT_REQ_DETAIL, OUT_PAGE
};
enum MediaKey : uint8_t { MK_PLAYPAUSE = 0, MK_NEXT, MK_PREV };
struct OutMsg { OutType type; int16_t a; int16_t b; };

// ---------------------------------------------------------------------------
//  Metric payloads
// ---------------------------------------------------------------------------
//  "summary" arrives every few hundred ms, "detail" roughly once per second.
//  A value of -1 means "not available".
// ---------------------------------------------------------------------------
struct Metrics {
  // summary
  int16_t cpu  = -1, ram  = -1, gpu  = -1, vram = -1;   // percent 0..100
  // detail (fixed order matches the E, protocol line - see proto.h/README)
  int16_t cpuTempX10   = -1;   // CPU temperature      * 10
  int16_t cpuPowerX10  = -1;   // CPU package power    * 10
  int16_t cpuFreqMhz   = -1;   // CPU clock, MHz
  int16_t cpuCores     = -1;   // logical core count
  int16_t ramUsedMb    = -1;
  int16_t ramTotalMb   = -1;
  int16_t gpuTempX10   = -1;   // GPU edge temperature * 10
  int16_t gpuHotX10    = -1;   // GPU hotspot          * 10
  int16_t gpuMemTempX10= -1;   // GPU memory temp      * 10
  int16_t gpuPowerX10  = -1;   // GPU board power      * 10
  int16_t gpuFan       = -1;   // GPU fan, percent
  int16_t vramUsedMb   = -1;
  int16_t vramTotalMb  = -1;
};

#define DETAIL_FIELDS 13

struct NowPlaying {
  char    title[40]  = "";
  char    artist[32] = "";
  uint8_t state      = 0;      // 0 none, 1 playing, 2 paused
};

// ---------------------------------------------------------------------------
//  Runtime state
// ---------------------------------------------------------------------------
struct Runtime {
  // link
  bool     pcUp        = false;
  bool     helloSeen   = false;
  uint32_t lastPcRxMs  = 0;

  // data from the PC
  Metrics    met;
  int16_t    volume    = -1;   // 0..100, -1 unknown
  bool       mute      = false;
  NowPlaying np;

  // UI
  Mode     mode       = MODE_IDLE;
  Page     menuPage   = PAGE_MONITOR;   // highlighted tile in the menu
  Page     page       = PAGE_MONITOR;   // page currently opened
  Comp     selComp    = COMP_CPU;       // selected monitor arc / detail target
  uint8_t  selMedia   = 0;              // highlighted media action
  uint8_t  gifIndex   = 0;              // animation currently playing
  uint32_t lastUserMs = 0;              // last user interaction (timeouts)
  bool     uiDirty    = true;           // force a full redraw on the next frame

  // audio reactive spectrum, fed by the PC ("A," messages)
  uint8_t  spec[AUDIO_BANDS] = { 0 };   // 0..100 per band
  uint8_t  pulse             = 0;       // beat envelope, 0..100
};

// ---------------------------------------------------------------------------
//  Globals + API
// ---------------------------------------------------------------------------
extern SemaphoreHandle_t gStateMtx;
extern QueueHandle_t     gInputQ;
extern QueueHandle_t     gOutQ;
extern Runtime           gState;

void stateInit(void);

static inline void stateLock(void)   { xSemaphoreTake(gStateMtx, portMAX_DELAY); }
static inline void stateUnlock(void) { xSemaphoreGive(gStateMtx); }

// Appliers (called by taskComm while the lock is NOT held - they lock internally)
void stateApplySummary(int cpu, int ram, int gpu, int vram);
void stateApplyDetail(const int16_t* v);
void stateApplyVolume(int vol, int mute);
void stateApplyNowPlaying(int state, const char* artist, const char* title);
void stateApplyAudio(const int16_t* v);   // AUDIO_BANDS levels + 1 pulse
void stateMarkPcSeen(void);

// Helpers
int16_t stateCompPercent(Comp c);       // locked read of a summary value
const char* compName(Comp c);           // "CPU" / "RAM" / "GPU" / "VRAM"
const char* pageName(Page p);           // "MONITOR" / "VOLUME" / "MEDIA" / "GALLERY"
