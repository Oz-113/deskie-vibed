/**
 * ============================================================================
 *  config.h  -  Central configuration for the UART Volume Controller
 * ============================================================================
 *  Everything you are likely to want to tweak lives in this one file:
 *    - pin map
 *    - animation (GIF) table + the per-animation colour palette
 *    - on-screen layout / geometry of the gauges (arcs)
 *    - timing constants
 *    - FreeRTOS task placement and priorities
 *
 *  The TFT_eSPI display pins are NOT here - they live in the library's
 *  User_Setup.h (GC9A01, HSPI: MOSI 11 / SCLK 12 / CS 10 / DC 14 / RST 15).
 *  Do not move them here, they must stay in User_Setup.h.
 * ============================================================================
 */
#pragma once

#include <stdint.h>

// ============================================================================
//  1. IDENTITY / SERIAL PROTOCOL
// ============================================================================
#define FW_NAME              "VolumeControllerUART"
#define FW_PROTOCOL_VERSION  1
#define UART_BAUD            921600     // USB-CDC ignores this, kept for a classic UART
#define RX_LINE_MAX          192        // longest accepted command line
#define RX_STALE_MS          1500       // no PC traffic for this long -> link marked down
#define TX_QUEUE_LEN         32

// ============================================================================
//  2. PIN MAP
// ============================================================================
// ---- Rotary encoder (quadrature, interrupt driven) -------------------------
#define PIN_ENC_A            5
#define PIN_ENC_B            4
#define PIN_ENC_BTN          17   // the encoder's own push switch (was "btnChangeAnim")

// ---- Dedicated media buttons ----------------------------------------------
#define PIN_BTN_PLAY         6
#define PIN_BTN_NEXT         7
#define PIN_BTN_PREV         8

// ---- Reserved "shortcut" buttons (hardware not fitted yet) -----------------
// Pulled up, active LOW, so an unwired pin simply reads HIGH and never fires.
// Set ENABLE_SHORTCUT_BUTTONS to 0 to reclaim the pins/footprint.
#define ENABLE_SHORTCUT_BUTTONS 1
#define PIN_SC_MONITOR       21   // jump straight to the resource monitor
#define PIN_SC_VOLUME        38   // jump straight to the volume page
#define PIN_SC_MEDIA         47   // jump straight to the media page
#define PIN_SC_GALLERY       48   // jump straight to the animation picker

// ============================================================================
//  3. ANIMATION TABLE + COLOUR PALETTE
// ============================================================================
//  Each entry maps to LittleFS files "<prefix>_<n>.raw" (240x240 RGB565,
//  little endian, as produced by the project's img_to_565.py).
//
//   accent    : main UI colour of the animation (outlines, gauge track)
//   highlight : brighter colour used for the SELECTED element / filled gauge
//   tint      : very dark colour used to fill gauge backgrounds
//
//  Tweak these freely - this is the single palette you asked for.
// ----------------------------------------------------------------------------
struct GifPreset {
  const char* prefix;    // LittleFS filename prefix, without the "_n.raw"
  uint8_t     frames;    // number of frames on disk (0 or 1 == single still)
  uint8_t     fps;       // target playback rate
  uint16_t    accent;    // main colour
  uint16_t    highlight; // selected / active colour
  uint16_t    tint;      // dark background fill for gauges
};

#define GIF_COUNT 5
static const GifPreset GIF_TABLE[GIF_COUNT] = {
  // prefix            frames fps   accent   highlight tint
  { "cirilla",           19,  20,  0x738E,  0xC618,  0x18E3 }, // silver / neutral
  { "2b",                15,  60,  0x501F,  0x9C9F,  0x1004 }, // violet
  { "lucycigarette",     24,  20,  0xB81F,  0xFBBF,  0x2804 }, // pink
  { "lucydavidmoon",      1,   5,  0xC617,  0xFEA0,  0x2980 }, // amber
  { "lucybg",             1,   5,  0x4FE4,  0x97F3,  0x0A61 }, // green
};

// ============================================================================
//  4. LAYOUT / GEOMETRY  (240x240 round GC9A01)
// ============================================================================
#define SCR_W                240
#define SCR_H                240
#define SCR_CX               (SCR_W / 2)
#define SCR_CY               (SCR_H / 2)

// ---- Resource monitor: four gauges arranged around the ring ---------------
#define MON_GAP_DEG          28                       // gap between gauges
#define MON_SWEEP_DEG        (90 - MON_GAP_DEG)       // angular length of each gauge
#define MON_R_OUTER          108                      // outer radius of a gauge
#define MON_R_INNER          92                       // inner radius of a gauge
#define MON_LABEL_R          78                       // radius of the small label
#define MON_TEXT_R           58                       // radius where the "%" is printed
// gauge centres (deg): 0 = 6 o'clock, clockwise  (matches TFT_eSPI)
#define MON_ANG_CPU          180                      // top
#define MON_ANG_RAM          270                      // right
#define MON_ANG_GPU            0                      // bottom
#define MON_ANG_VRAM          90                      // left

// ---- Big single gauge used by detail / volume pages ------------------------
#define BIG_R_OUTER          112
#define BIG_R_INNER          97
#define BIG_START_DEG        45                       // 7:30 position
#define BIG_END_DEG          315                      // 4:30 position (drawn clockwise)

// ---- Segmented ring used by the animation (gallery) picker -----------------
#define GAL_R_OUTER          110
#define GAL_R_INNER          98
#define GAL_SEG_GAP_DEG      6

// ---- Menu (page selector) tiles -------------------------------------------
#define MENU_TILE_W          66
#define MENU_TILE_H          66
#define MENU_TILE_R          14

// ============================================================================
//  5. TIMING
// ============================================================================
#define UI_IDLE_TIMEOUT_MS      8000    // auto-return to the idle animation
#define UI_DETAIL_TIMEOUT_MS   20000    // detail page lingers longer
#define IDLE_SPIN_PERIOD_MS     8000    // one full turn of the now-playing ornament
#define LONGPRESS_MS             600    // encoder button: >= this == "back"
#define BTN_DEBOUNCE_MS           25
#define BTN_MEDIA_REPEAT_MS      220    // debounce for the dedicated media buttons
#define DETAIL_REQ_INTERVAL_MS  1000    // how often we ask the PC for full detail

// ============================================================================
//  6. FREERTOS / DUAL-CORE PLACEMENT
// ============================================================================
//   core 0 : comms (UART) + input (encoder/buttons)   -> latency sensitive, light
//   core 1 : renderer (LittleFS + TFT)                -> heavy, must not be starved
// ----------------------------------------------------------------------------
#define CORE_COMM            0
#define CORE_RENDER          1
#define TASK_PRIO_COMM       4
#define TASK_PRIO_INPUT      5
#define TASK_PRIO_RENDER     6
#define TASK_STACK_COMM      4096
#define TASK_STACK_INPUT     3072
#define TASK_STACK_RENDER    8192

// ============================================================================
//  7. AUDIO REACTIVE SPECTRUM
// ============================================================================
//  The PC captures the system audio (WASAPI loopback) and sends eight
//  log-spaced band levels plus a beat pulse as "A,<b0>..<b7>,<pulse>".
//  The board only draws them as radial bars - all the DSP stays on the PC.
// ----------------------------------------------------------------------------
#define ENABLE_SPECTRUM      1        // 0 = ignore the A, messages and draw nothing
#define AUDIO_BANDS          8        // must match AUDIO_BANDS in controller.py
#define SPEC_SEGMENTS        36       // radial bars drawn around the ring
#define SPEC_GAP_DEG         2        // gap between bars, in degrees
#define SPEC_R_INNER         92       // inner radius of a bar (length 0)
#define SPEC_R_MAX           118      // radius when a band is at 100 %
#define SPEC_GAMMA           0.62f    // <1 boosts mid levels into visible bars
#define SPEC_START_DEG       80       // arc range for the bars, 0 = 6 o'clock,
#define SPEC_END_DEG         280      // clockwise. The upper half leaves the
                                      // bottom free for the now-playing ticker.
#define SPEC_PULSE_R         84       // radius of the beat pulse ring (inside bars)
#define SPEC_PULSE_W         3        // extra thickness of that ring at pulse 100
