// linklite-bar/config.h
// Pin map, wire protocol, and tuning constants. See PLAN.md for the design behind these.
//
// NOTE: LinkLitePacket / MsgType / WIFI_CHANNEL / STEP_SIZE_PERCENT must stay identical to the
// copies in ../linklite-controller/linklite-controller.ino until the two sketches share a header
// (PLAN.md §9 / §12 — there's no shared-library setup yet, so this is a manually-kept invariant).

#pragma once

#include <Arduino.h>

// ---- GPIO map (CLAUDE.md "Light bar" table) --------------------------------------------------
#define PIN_BUTTON_1  20
#define PIN_BUTTON_2  21
#define PIN_USER_LED  22
#define PIN_MAIN_LEDS 23 // 23 for c6 and 0 for c3

// ---- LEDC (PWM) config (PLAN.md §4) -----------------------------------------------------------
#define LEDC_FREQ_HZ     25000
#define LEDC_RES_BITS    8
#define LEDC_MAX_DUTY    ((1 << LEDC_RES_BITS) - 1)  // max raw duty for the configured resolution

// GAMMA_LUT below is precomputed at a fixed 12-bit domain (0..4095) regardless of LEDC_RES_BITS —
// see the LUT comment. Rescale its output to LEDC_MAX_DUTY at runtime so changing LEDC_RES_BITS
// (or LEDC_FREQ_HZ, which constrains max achievable resolution) can't desync percent from duty.
#define GAMMA_LUT_MAX_DUTY 4095

// Hard ceiling on output duty cycle, as a percentage of LEDC_MAX_DUTY. Lets max LED current be
// capped in firmware (thermal/driver limits) without touching the gamma curve or brightness math.
#define LEDC_MAX_DUTY_PERCENT 100

// ---- Brightness domain (PLAN.md §2) -----------------------------------------------------------
#define BRIGHTNESS_MIN_PCT   0
#define BRIGHTNESS_MAX_PCT   100
#define BRIGHTNESS_DEFAULT_PCT 25   // used only if NVS has no stored value and no peers respond
#define STEP_SIZE_PERCENT    2      // must match linklite-controller's per-detent step

// ---- Radio (PLAN.md §5 step 3, §10) -----------------------------------------------------------
#define WIFI_CHANNEL     1          // must match linklite-controller; ESP-NOW requires same channel
#define DEFAULT_GROUP_ID 0

// ---- Timing (PLAN.md §5, §6) ------------------------------------------------------------------
#define BOOT_QUERY_TIMEOUT_MS   500
#define NVS_WRITE_QUIESCE_MS    2000
#define BUTTON_DEBOUNCE_MS      30
#define BUTTON_LONG_PRESS_MS    2000
#define ACTIVITY_FLASH_MS       30

// ---- Wire protocol (PLAN.md §7) ---------------------------------------------------------------
enum MsgType : uint8_t {
	MSG_DELTA  = 0x01,  // relative brightness step, from a controller
	MSG_TOGGLE = 0x02,  // on/off toggle, from a controller
	MSG_QUERY  = 0x03,  // "what's the current level?" — sent by a bar on boot
	MSG_STATE  = 0x04,  // response to MSG_QUERY, or future heartbeat (PLAN.md §7.3)
};

enum SenderType : uint8_t {
	SENDER_BAR        = 0,
	SENDER_CONTROLLER = 1,
};

typedef struct __attribute__((packed)) {
	uint8_t  groupId;
	uint8_t  msgType;     // MsgType
	uint8_t  senderType;  // SenderType
	uint32_t seq;
	int8_t   delta;       // MSG_DELTA / MSG_TOGGLE payload, percent points
	uint8_t  level;       // MSG_STATE payload, absolute percent 0-100
} LinkLitePacket;         // 9 bytes, __packed__ so both sketches agree on wire layout

// ---- Gamma LUT (PLAN.md §4) --------------------------------------------------------------------
// duty = round(4095 * (percent/100)^2.2), percent = 0..100. Precomputed — do NOT call pow() at
// runtime, the C6 (RISC-V) has no hardware FPU and pow() is a soft-float library call.
// TODO(plan §4): this table is a placeholder derived from the formula above; re-derive/tune once
// LEDs are on the bench and perceived linearity can actually be judged by eye.
extern const uint16_t GAMMA_LUT[101];
