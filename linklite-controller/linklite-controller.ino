// linklite-controller.ino
// ESP32-C3 remote: EC11 rotary encoder + push button, ESP-NOW to the bars.
// Stateless emitter of intent only (CLAUDE.md "Sync model") — never tracks or assumes a
// brightness level, just emits relative deltas and a toggle event, then goes back to sleep.
//
// Lifecycle: light sleep (GPIO wake source) -> wake on encoder/button edge -> short awake
// session polling further motion + debouncing the button -> emit ESP-NOW packet(s) -> idle
// timeout -> back to light sleep. Light sleep retains RAM/CPU state, so plain (non-RTC) statics
// survive across sleep cycles and resume is fast.

#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <RotaryEncoder.h>
#include "config.h"

static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

uint32_t txSeq = 0;

// Encoder decode/debounce lives in the RotaryEncoder library (Gray-code latch state machine) —
// see setup() for construction. Polled from runAwakeSession()'s loop rather than driven by an
// A/B interrupt: RotaryEncoder::tick() isn't IRAM-resident, and calling it from an IRAM ISR at
// the moment flash cache happens to be disabled (e.g. during esp_wifi_set_channel(), which
// runAwakeSession() calls on every wake) faults hard enough to wedge the chip until physical
// reset. The awake session already polls every 5ms for button debounce, so folding the encoder
// into that same loop costs nothing and avoids the ISR path entirely.
RotaryEncoder *encoder = nullptr;

void onEspNowSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
	if (status != ESP_NOW_SEND_SUCCESS) {
		Serial.println("ESP-NOW send failed"); // surfaces the exact failure this bug report needs
	}
}

void sendPacket(MsgType type, int8_t delta, uint8_t level) {
	LinkLitePacket pkt = {};
	pkt.groupId = DEFAULT_GROUP_ID;
	pkt.msgType = type;
	pkt.senderType = SENDER_CONTROLLER;
	pkt.seq = txSeq++;
	pkt.delta = delta;
	pkt.level = level;
	esp_now_send(BROADCAST_MAC, (uint8_t *)&pkt, sizeof(pkt));
}

void configureWakeSources() {
	gpio_wakeup_enable((gpio_num_t)PIN_ENC_A, GPIO_INTR_LOW_LEVEL);
	gpio_wakeup_enable((gpio_num_t)PIN_ENC_B, GPIO_INTR_LOW_LEVEL);
	gpio_wakeup_enable((gpio_num_t)PIN_ENC_BTN, GPIO_INTR_LOW_LEVEL);
	esp_sleep_enable_gpio_wakeup();
}

void enterLightSleepUntilWake() {
	esp_light_sleep_start();
}

// Runs from wake until IDLE_TIMEOUT_MS of no further encoder/button activity, then returns so
// the caller can go back to sleep. Batches encoder motion into as few ESP-NOW sends as possible.
void runAwakeSession() {
	// The C3's Wi-Fi/RF state isn't guaranteed to come back exactly as it was after
	// esp_light_sleep_start() — in practice the channel can end up unset/wrong on wake, which
	// silently breaks ESP-NOW TX until the whole device (and its Wi-Fi driver) is reset. Since a
	// full reset is exactly what "fixes" this in the field, reassert the channel every wake
	// rather than trusting it survived sleep.
	esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

	encoder->tick();
	long lastPosition = encoder->getPosition();

	bool btnDebounced = digitalRead(PIN_ENC_BTN);
	bool btnLastRaw = btnDebounced;
	uint32_t btnLastChangeMs = millis();

	uint32_t lastActivityMs = millis();

	while (millis() - lastActivityMs < IDLE_TIMEOUT_MS) {
		encoder->tick();
		long position = encoder->getPosition();
		long steps = position - lastPosition;
		lastPosition = position;

		if (steps != 0) {
			int32_t percentDelta = (int32_t)steps * STEP_SIZE_PERCENT;
			if (percentDelta > 127) percentDelta = 127;
			if (percentDelta < -127) percentDelta = -127;
			sendPacket(MSG_DELTA, (int8_t)percentDelta, 0);
			lastActivityMs = millis();
		}

		bool raw = digitalRead(PIN_ENC_BTN);
		uint32_t now = millis();
		if (raw != btnLastRaw) {
			btnLastRaw = raw;
			btnLastChangeMs = now;
		}
		if (now - btnLastChangeMs > BUTTON_DEBOUNCE_MS && btnDebounced != raw) {
			btnDebounced = raw;
			if (btnDebounced == HIGH) { // released (active-low button, so HIGH = released)
				sendPacket(MSG_TOGGLE, 0, 0); // on/off toggle, per project decision
				lastActivityMs = now;
			}
		}

		delay(5);
	}
}

void setup() {
	Serial.begin(115200);

	// RotaryEncoder's 2-pin constructor configures PIN_ENC_A/B as INPUT_PULLUP itself. FOUR3
	// matches our wiring: both pins idle HIGH (via pull-up) at rest, so external position only
	// latches at that all-HIGH state — see LatchMode comments in RotaryEncoder.h.
	encoder = new RotaryEncoder(PIN_ENC_A, PIN_ENC_B, RotaryEncoder::LatchMode::FOUR3);

	pinMode(PIN_ENC_BTN, INPUT_PULLUP);

	WiFi.mode(WIFI_STA);
	WiFi.disconnect();
	// Wi-Fi's own power-save state machine (modem sleep, DTIM-driven wake, etc.) is meant to
	// coordinate with an AP connection we don't have here, and fighting it with our own explicit
	// esp_light_sleep_start() calls is a known source of the radio not coming back cleanly after
	// sleep. We already own all the power saving via light sleep — keep Wi-Fi's PS state machine
	// out of it entirely so the radio is always in a known-good state whenever we're awake.
	esp_wifi_set_ps(WIFI_PS_NONE);
	// Fixed channel: an unassociated STA otherwise reuses whatever channel is cached in Wi-Fi
	// NVS, which can silently drift out of sync with the bars (must match linklite-bar/config.h).
	esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

	if (esp_now_init() != ESP_OK) {
		Serial.println("ESP-NOW init failed");
		// No user-facing indicator on this board (no status LED per CLAUDE.md's controller
		// pinout) - nothing further to do here; retry logic is out of scope for this skeleton.
	} else {
		esp_now_register_send_cb(onEspNowSent);
		esp_now_peer_info_t peer = {};
		memcpy(peer.peer_addr, BROADCAST_MAC, 6);
		peer.channel = WIFI_CHANNEL;
		peer.encrypt = false; // matches linklite-bar; no ESP-NOW encryption in v1
		esp_now_add_peer(&peer);
	}

	configureWakeSources();
}

void loop() {
	enterLightSleepUntilWake();

	esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
	if (cause != ESP_SLEEP_WAKEUP_GPIO) return; // spurious wake; loop back to sleep immediately

	runAwakeSession();
}
