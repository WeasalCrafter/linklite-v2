// linklite-bar.ino
// Authoritative brightness node — see PLAN.md for the full design this implements.
// Settled pieces (GPIO/LEDC, NVS, ESP-NOW init/send/recv, delta/toggle application, boot query)
// are implemented. Open design points are marked `// TODO(plan §n)` and left unimplemented —
// see PLAN.md §14 for the list.

#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <Preferences.h>
#include "config.h"

const uint16_t GAMMA_LUT[101] = {
	0, 0, 1, 2, 3, 6, 8, 12, 16, 20,
	26, 32, 39, 46, 54, 63, 73, 83, 94, 106,
	119, 132, 146, 161, 177, 194, 211, 230, 249, 269,
	290, 311, 334, 357, 382, 407, 433, 460, 487, 516,
	545, 576, 607, 640, 673, 707, 742, 778, 815, 852,
	891, 931, 972, 1013, 1056, 1099, 1144, 1189, 1235, 1283,
	1331, 1380, 1431, 1482, 1534, 1587, 1642, 1697, 1753, 1810,
	1868, 1928, 1988, 2049, 2111, 2175, 2239, 2304, 2371, 2438,
	2506, 2576, 2646, 2718, 2790, 2864, 2939, 3014, 3091, 3169,
	3248, 3328, 3409, 3491, 3574, 3658, 3743, 3830, 3917, 4005,
	4095,
};

static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

Preferences prefs;

volatile uint8_t brightnessPct = BRIGHTNESS_DEFAULT_PCT;
uint8_t lastNonZeroPct = BRIGHTNESS_DEFAULT_PCT;
uint8_t groupId = DEFAULT_GROUP_ID;
uint32_t txSeq = 0;

// Rate-limited NVS persistence (PLAN.md §6)
bool nvsDirty = false;
uint32_t lastChangeMs = 0;

// User LED state machine (PLAN.md §8)
enum UserLedMode { LED_BOOT_WAIT, LED_NORMAL, LED_FLASH, LED_PAIRING, LED_FAULT };
UserLedMode userLedMode = LED_BOOT_WAIT;
uint32_t userLedFlashUntilMs = 0;

// Recv callback runs in the Wi-Fi task context — keep it to a copy into this small ring buffer,
// do real handling in loop() (PLAN.md §6).
#define RX_QUEUE_LEN 8
LinkLitePacket rxQueue[RX_QUEUE_LEN];
volatile uint8_t rxHead = 0, rxTail = 0;

// Button debounce state (PLAN.md §9)
struct ButtonState {
	uint8_t pin;
	bool lastRaw;
	bool debounced;
	uint32_t lastChangeMs;
	uint32_t pressStartMs;
	bool longPressFired;
};
ButtonState button1 = {PIN_BUTTON_1, true, true, 0, 0, false};
ButtonState button2 = {PIN_BUTTON_2, true, true, 0, 0, false};

bool bootQueryPending = false;
uint32_t bootQueryStartMs = 0;
bool bootQueryAnswered = false;

void setBrightness(uint8_t pct);
void applyDelta(int8_t delta);
void applyToggle();
void sendPacket(MsgType type, int8_t delta, uint8_t level);
void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len);
void handleRxQueue();
void handleButtons();
void updateUserLed();
void persistIfDirty();
void flashActivity();

void setup() {
	Serial.begin(115200);

	pinMode(PIN_BUTTON_1, INPUT_PULLUP);
	pinMode(PIN_BUTTON_2, INPUT_PULLUP);
	pinMode(PIN_USER_LED, OUTPUT);
	digitalWrite(PIN_USER_LED, LOW);

	ledcAttach(PIN_MAIN_LEDS, LEDC_FREQ_HZ, LEDC_RES_BITS);

	// Step 2 (PLAN.md §5): load persisted state, drive output immediately.
	prefs.begin("linklite", false);
	brightnessPct = prefs.getUChar("pct", BRIGHTNESS_DEFAULT_PCT);
	groupId = prefs.getUChar("grp", DEFAULT_GROUP_ID);
	lastNonZeroPct = brightnessPct > 0 ? brightnessPct : BRIGHTNESS_DEFAULT_PCT;
	setBrightness(brightnessPct);

	// Step 3 (PLAN.md §5): Wi-Fi STA pinned to a fixed channel, ESP-NOW up.
	WiFi.mode(WIFI_STA);
	WiFi.disconnect();
	esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

	if (esp_now_init() != ESP_OK) {
		Serial.println("ESP-NOW init failed");
		userLedMode = LED_FAULT;
	} else {
		esp_now_peer_info_t peer = {};
		memcpy(peer.peer_addr, BROADCAST_MAC, 6);
		peer.channel = WIFI_CHANNEL;
		peer.encrypt = false; // PLAN.md §10: no encryption in v1, flagged as an open item
		esp_now_add_peer(&peer);
		esp_now_register_recv_cb(onEspNowRecv);

		// Step 4 (PLAN.md §5): ask peers for current state before trusting NVS.
		sendPacket(MSG_QUERY, 0, 0);
		bootQueryPending = true;
		bootQueryStartMs = millis();
	}
}

void loop() {
	handleRxQueue();
	handleButtons();

	if (bootQueryPending && millis() - bootQueryStartMs >= BOOT_QUERY_TIMEOUT_MS) {
		bootQueryPending = false;
		userLedMode = LED_NORMAL;
		// If bootQueryAnswered, onEspNowRecv already adopted a peer's state (PLAN.md §5 step 4).
		// If not, we keep the NVS/default value that's already applied.
	}

	// TODO(plan §7.3): periodic MSG_STATE heartbeat + quiescent drift reconciliation.
	// Deliberately unimplemented — needs bench validation with induced packet loss first.

	persistIfDirty();
	updateUserLed();
}

void setBrightness(uint8_t pct) {
	if (pct > BRIGHTNESS_MAX_PCT) pct = BRIGHTNESS_MAX_PCT;
	brightnessPct = pct;
	uint32_t duty = 0;
	if (pct != 0) { // exact zero at 0%, PLAN.md §4
		// GAMMA_LUT is a fixed 12-bit (0..4095) curve; rescale to the actual LEDC resolution,
		// then apply the configured max-duty ceiling.
		duty = (uint32_t)GAMMA_LUT[pct] * LEDC_MAX_DUTY / GAMMA_LUT_MAX_DUTY;
		duty = duty * LEDC_MAX_DUTY_PERCENT / 100;
	}
	ledcWrite(PIN_MAIN_LEDS, duty);
	Serial.print(pct);
	Serial.print(" ");
	Serial.println(duty);
	nvsDirty = true;
	lastChangeMs = millis();
	if (pct > 0) lastNonZeroPct = pct;
}

void applyDelta(int8_t delta) {
	int16_t next = (int16_t)brightnessPct + delta;
	if (next < BRIGHTNESS_MIN_PCT) next = BRIGHTNESS_MIN_PCT;
	if (next > BRIGHTNESS_MAX_PCT) next = BRIGHTNESS_MAX_PCT;
	setBrightness((uint8_t)next);
	flashActivity();
}

void applyToggle() {
	if (brightnessPct > 0) {
		setBrightness(0);
	} else {
		setBrightness(lastNonZeroPct);
	}
	flashActivity();
}

void sendPacket(MsgType type, int8_t delta, uint8_t level) {
	LinkLitePacket pkt = {};
	pkt.groupId = groupId;
	pkt.msgType = type;
	pkt.senderType = SENDER_BAR;
	pkt.seq = txSeq++;
	pkt.delta = delta;
	pkt.level = level;
	esp_now_send(BROADCAST_MAC, (uint8_t *)&pkt, sizeof(pkt));
}

// Runs in Wi-Fi task context — must stay short and non-blocking (PLAN.md §6).
void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
	if (len != sizeof(LinkLitePacket)) return;
	uint8_t nextTail = (rxTail + 1) % RX_QUEUE_LEN;
	if (nextTail == rxHead) return; // queue full, drop — better than blocking the Wi-Fi task
	memcpy(&rxQueue[rxTail], data, sizeof(LinkLitePacket));
	rxTail = nextTail;
}

void handleRxQueue() {
	while (rxHead != rxTail) {
		LinkLitePacket pkt = rxQueue[rxHead];
		rxHead = (rxHead + 1) % RX_QUEUE_LEN;

		if (pkt.groupId != groupId) continue; // PLAN.md §7: isolate separate installations

		switch (pkt.msgType) {
			case MSG_DELTA:
				applyDelta(pkt.delta);
				break;
			case MSG_TOGGLE:
				applyToggle();
				break;
			case MSG_QUERY:
				// Another bar booting and asking for current state (PLAN.md §5 step 4).
				sendPacket(MSG_STATE, 0, brightnessPct);
				break;
			case MSG_STATE:
				if (bootQueryPending && !bootQueryAnswered) {
					// First reply wins (PLAN.md §5 step 4).
					bootQueryAnswered = true;
					setBrightness(pkt.level);
				}
				// TODO(plan §7.3): quiescent drift reconciliation against unsolicited/heartbeat
				// MSG_STATE — not implemented; a stray MSG_STATE outside boot is ignored for now.
				break;
			default:
				break;
		}
	}
}

void handleButtons() {
	uint32_t now = millis();

	// Button 1: implemented per PLAN.md §9 proposed default.
	bool raw1 = digitalRead(PIN_BUTTON_1);
	if (raw1 != button1.lastRaw) {
		button1.lastRaw = raw1;
		button1.lastChangeMs = now;
	}
	if (now - button1.lastChangeMs > BUTTON_DEBOUNCE_MS && button1.debounced != raw1) {
		button1.debounced = raw1;
		if (button1.debounced == LOW) { // pressed (active low)
			button1.pressStartMs = now;
			button1.longPressFired = false;
		} else { // released
			if (!button1.longPressFired) {
				applyToggle();
				sendPacket(MSG_TOGGLE, 0, 0); // broadcast so peers follow (PLAN.md §9)
			}
		}
	}
	if (button1.debounced == LOW && !button1.longPressFired &&
	    now - button1.pressStartMs >= BUTTON_LONG_PRESS_MS) {
		button1.longPressFired = true;
		// TODO(plan §9): "identify" — fast-blink user LED + pulse main LEDs. Not implemented.
	}

	// Button 2: debounced and tracked, action is entirely open (PLAN.md §9 — pairing/groupId).
	bool raw2 = digitalRead(PIN_BUTTON_2);
	if (raw2 != button2.lastRaw) {
		button2.lastRaw = raw2;
		button2.lastChangeMs = now;
	}
	if (now - button2.lastChangeMs > BUTTON_DEBOUNCE_MS && button2.debounced != raw2) {
		button2.debounced = raw2;
		// TODO(plan §9): group/pairing management. No action implemented yet.
	}
}

void flashActivity() {
	if (userLedMode == LED_NORMAL) {
		userLedMode = LED_FLASH;
		userLedFlashUntilMs = millis() + ACTIVITY_FLASH_MS;
	}
}

void updateUserLed() {
	uint32_t now = millis();
	switch (userLedMode) {
		case LED_BOOT_WAIT:
			digitalWrite(PIN_USER_LED, (now / 250) % 2); // slow blink, ~2 Hz
			break;
		case LED_NORMAL:
			digitalWrite(PIN_USER_LED, HIGH);
			break;
		case LED_FLASH:
			digitalWrite(PIN_USER_LED, LOW);
			if (now >= userLedFlashUntilMs) userLedMode = LED_NORMAL;
			break;
		case LED_PAIRING:
			// TODO(plan §9): fast blink during pairing mode. Mode never entered yet.
			digitalWrite(PIN_USER_LED, (now / 100) % 2);
			break;
		case LED_FAULT:
			digitalWrite(PIN_USER_LED, (now / 150) % 6 < 3); // crude triple-flash-ish pattern
			break;
	}
}

void persistIfDirty() {
	if (nvsDirty && millis() - lastChangeMs >= NVS_WRITE_QUIESCE_MS) {
		prefs.putUChar("pct", brightnessPct);
		prefs.putUChar("grp", groupId);
		nvsDirty = false;
	}
}
