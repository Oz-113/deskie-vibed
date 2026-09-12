/**
 * ============================================================================
 *  comms.h  -  UART (USB-CDC) line protocol, pinned to core 0
 * ============================================================================
 *  PC -> ESP :  H,<ver> | V,<vol>,<mute> | S,<cpu>,<ram>,<gpu>,<vram>
 *               E,<13 ints> | T,<state>,<artist>|<title> | P
 *  ESP -> PC :  H,<ver> | VOL,<+n|-n> | VSET,<n> | MUTE,<0|1>
 *               MK,<PP|N|P> | REQ,<C|R|G|V> | PG,<MONITOR|VOLUME|MEDIA|GALLERY> | PONG
 *
 *  See README.md for the full description of every field.
 * ============================================================================
 */
#pragma once

#include <Arduino.h>
#include "state.h"

void commInit(void);
void taskComm(void* arg);

// Queue a message for the PC (thread safe, never blocks)
void commSend(OutType type, int16_t a = 0, int16_t b = 0);

// Convenience wrappers used by the UI
void commVolStep(int8_t dir);
void commVolSet(int pct);
void commMute(bool on);
void commMedia(MediaKey k);
void commReqDetail(Comp c);
void commPage(Page p);
