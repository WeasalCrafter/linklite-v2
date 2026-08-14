// linklite-controller/config.h
// Pin map, wire protocol, and tuning constants for the EC11 remote.
//
// NOTE: LinkLitePacket / MsgType / WIFI_CHANNEL / STEP_SIZE_PERCENT must stay identical to the
// copies in ../linklite-bar/config.h until the two sketches share a header — there's no
// shared-library setup yet, so this is a manually-kept invariant (see linklite-bar/PLAN.md §12).

#pragma once

#include <Arduino.h>

// ---- GPIO map (CLAUDE.md "Controller" table) --------------------------------------------------
// Flagged in CLAUDE.md as "not yet fixed" — these are usable as light-sleep GPIO wake sources on
// the C3 (any GPIO works for light sleep, unlike deep sleep's RTC-IO-only restriction), so they're
// kept as documented until there's a reason to move them.
#define PIN_ENC_A 2
#define PIN_ENC_B 1
#define PIN_ENC_BTN 0  //3

// ---- Brightness domain (must match linklite-bar/config.h) --------------------------------------
#define STEP_SIZE_PERCENT 2  // percent points emitted per encoder detent

// ---- Radio (must match linklite-bar/config.h) ---------------------------------------------------
#define WIFI_CHANNEL 1
#define DEFAULT_GROUP_ID 0

// ---- Timing --------------------------------------------------------------------------------------
// Encoder debounce is handled by the RotaryEncoder library's Gray-code latch state machine
// (LatchMode::FOUR3) rather than a time-based gate — see linklite-controller.ino.
#define BUTTON_DEBOUNCE_MS 30
#define IDLE_TIMEOUT_MS 250  // no activity for this long -> back to light sleep

// ---- Wire protocol (must match linklite-bar/config.h) --------------------------------------------
enum MsgType : uint8_t {
	MSG_DELTA = 0x01,
	MSG_TOGGLE = 0x02,
	MSG_QUERY = 0x03,
	MSG_STATE = 0x04,
};

enum SenderType : uint8_t {
	SENDER_BAR = 0,
	SENDER_CONTROLLER = 1,
};

typedef struct __attribute__((packed)) {
	uint8_t groupId;
	uint8_t msgType;
	uint8_t senderType;
	uint32_t seq;
	int8_t delta;
	uint8_t level;
} LinkLitePacket;  // 9 bytes
