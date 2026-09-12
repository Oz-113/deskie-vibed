/**
 * ============================================================================
 *  VolumeControllerUART.ino  -  ESP32-S3 (N16R8) UART volume controller
 * ============================================================================
 *  A rewrite of the original USB-HID volume controller:
 *    - no USB HID   : a line based UART protocol talks to pc/controller.py
 *    - dual core    : core 0 = comms + input, core 1 = renderer
 *    - arc UI       : CPU / RAM / GPU / VRAM gauges plus volume and media pages
 *
 *  See README.md for the wiring, the protocol and the PC-side tool.
 *
 *  NOTE: the display pins live in the TFT_eSPI User_Setup.h (GC9A01) and the
 *        frames live in LittleFS - neither is touched by this project.
 * ============================================================================
 */
#include "config.h"
#include "state.h"
#include "display.h"
#include "comms.h"
#include "input.h"
#include "anim.h"
#include "ui.h"

#include <LittleFS.h>

// ---------------------------------------------------------------------------
//  Shared animation player (owned by the render task)
// ---------------------------------------------------------------------------
static Animation gAnim;

// ===========================================================================
//  Core 1 : renderer  (animation + UI -> panel)
// ===========================================================================
static void taskRender(void* arg) {
  (void)arg;

  for (;;) {
    const bool loaded = gAnim.tick(gFrameBuf);        // LittleFS -> frame buffer (fps gated)
    if (uiRender(gAnim, loaded)) displayPush();       // UI -> sprite -> panel
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ===========================================================================
//  Storage report (setup only - never run after the PC link is live)
// ===========================================================================
static void reportStorage(void) {
  const size_t total = LittleFS.totalBytes();
  const size_t used  = LittleFS.usedBytes();
  const size_t freeB = total - used;

  Serial.println("--- Storage ---");
  Serial.printf("Total : %.2f MB\n", total / (1024.0 * 1024.0));
  Serial.printf("Used  : %.2f MB\n", used  / (1024.0 * 1024.0));
  Serial.printf("Free  : %.2f MB (~%d frames)\n",
                freeB / (1024.0 * 1024.0), (int)(freeB / 115200));
}

// ===========================================================================
//  setup
// ===========================================================================
void setup() {
  Serial.begin(UART_BAUD);
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);                // never block on USB CDC writes
#endif
  Serial.printf("\n%s  protocol %d\n", FW_NAME, FW_PROTOCOL_VERSION);

  // 1. IPC primitives must exist before any task is created
  stateInit();

  // 2. LittleFS (the animation frames)
  if (!LittleFS.begin(true)) Serial.println("LittleFS mount FAILED");
  else                       reportStorage();

  // 3. display: panel + 16bpp sprite + the two PSRAM frame buffers
  if (!displayInit()) Serial.println("displayInit FAILED (PSRAM?)");

  // 4. peripherals and the UART protocol
  inputInit();
  commInit();

  // 5. animation + UI state
  gAnim.begin(0);
  uiInit(&gAnim);

  // 6. paint the first frame so the panel is never blank
  const bool firstFrame = gAnim.tick(gFrameBuf);
  if (uiRender(gAnim, firstFrame)) displayPush();

  // 7. dual-core task layout
  xTaskCreatePinnedToCore(taskComm,   "comm",   TASK_STACK_COMM,   nullptr,
                          TASK_PRIO_COMM,   nullptr, CORE_COMM);
  xTaskCreatePinnedToCore(taskInput,  "input",  TASK_STACK_INPUT,  nullptr,
                          TASK_PRIO_INPUT,  nullptr, CORE_COMM);
  xTaskCreatePinnedToCore(taskRender, "render", TASK_STACK_RENDER, nullptr,
                          TASK_PRIO_RENDER, nullptr, CORE_RENDER);
}

// ===========================================================================
//  loop : the Arduino loop task only idles - all work happens in the pinned
//         FreeRTOS tasks created in setup().
// ===========================================================================
void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
