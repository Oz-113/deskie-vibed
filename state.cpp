/**
 * ============================================================================
 *  state.cpp  -  Shared runtime state, mutex and queues
 * ============================================================================
 */
#include "state.h"
#include <string.h>

// ---------------------------------------------------------------------------
//  Globals
// ---------------------------------------------------------------------------
SemaphoreHandle_t gStateMtx = nullptr;
QueueHandle_t     gInputQ   = nullptr;
QueueHandle_t     gOutQ     = nullptr;
Runtime           gState;

// ---------------------------------------------------------------------------
//  Setup
// ---------------------------------------------------------------------------
void stateInit(void) {
  gStateMtx = xSemaphoreCreateMutex();
  gInputQ   = xQueueCreate(16, sizeof(InputEvent));
  gOutQ     = xQueueCreate(TX_QUEUE_LEN, sizeof(OutMsg));
  gState.lastUserMs = millis();
}

// ---------------------------------------------------------------------------
//  Link
// ---------------------------------------------------------------------------
void stateMarkPcSeen(void) {
  stateLock();
  gState.lastPcRxMs = millis();
  gState.pcUp       = true;
  stateUnlock();
}

// ---------------------------------------------------------------------------
//  Data appliers (called from taskComm)
// ---------------------------------------------------------------------------
void stateApplySummary(int cpu, int ram, int gpu, int vram) {
  stateLock();
  gState.met.cpu  = (int16_t)cpu;
  gState.met.ram  = (int16_t)ram;
  gState.met.gpu  = (int16_t)gpu;
  gState.met.vram = (int16_t)vram;
  stateUnlock();
}

void stateApplyDetail(const int16_t* v) {
  stateLock();
  Metrics& m = gState.met;
  m.cpuTempX10    = v[0];
  m.cpuPowerX10   = v[1];
  m.cpuFreqMhz    = v[2];
  m.cpuCores      = v[3];
  m.ramUsedMb     = v[4];
  m.ramTotalMb    = v[5];
  m.gpuTempX10    = v[6];
  m.gpuHotX10     = v[7];
  m.gpuMemTempX10 = v[8];
  m.gpuPowerX10   = v[9];
  m.gpuFan        = v[10];
  m.vramUsedMb    = v[11];
  m.vramTotalMb   = v[12];
  stateUnlock();
}

void stateApplyVolume(int vol, int mute) {
  stateLock();
  gState.volume = (int16_t)constrain(vol, -1, 100);
  gState.mute   = (mute != 0);
  stateUnlock();
}

void stateApplyAudio(const int16_t* v) {
  stateLock();
  for (uint8_t i = 0; i < AUDIO_BANDS; i++) {
    gState.spec[i] = (uint8_t)constrain(v[i], 0, 100);
  }
  gState.pulse = (uint8_t)constrain(v[AUDIO_BANDS], 0, 100);
  stateUnlock();
}

void stateApplyNowPlaying(int state, const char* artist, const char* title) {
  stateLock();
  gState.np.state = (uint8_t)state;
  strncpy(gState.np.artist, artist ? artist : "", sizeof(gState.np.artist) - 1);
  gState.np.artist[sizeof(gState.np.artist) - 1] = '\0';
  strncpy(gState.np.title, title ? title : "", sizeof(gState.np.title) - 1);
  gState.np.title[sizeof(gState.np.title) - 1] = '\0';
  stateUnlock();
}

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------
int16_t stateCompPercent(Comp c) {
  stateLock();
  int16_t v = -1;
  switch (c) {
    case COMP_CPU:  v = gState.met.cpu;  break;
    case COMP_RAM:  v = gState.met.ram;  break;
    case COMP_GPU:  v = gState.met.gpu;  break;
    case COMP_VRAM: v = gState.met.vram; break;
    default: break;
  }
  stateUnlock();
  return v;
}

const char* compName(Comp c) {
  switch (c) {
    case COMP_RAM:  return "RAM";
    case COMP_GPU:  return "GPU";
    case COMP_VRAM: return "VRAM";
    default:        return "CPU";
  }
}

const char* pageName(Page p) {
  switch (p) {
    case PAGE_VOLUME:  return "VOLUME";
    case PAGE_MEDIA:   return "MEDIA";
    case PAGE_GALLERY: return "GALLERY";
    default:           return "MONITOR";
  }
}
