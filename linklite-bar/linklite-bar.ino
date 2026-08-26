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

// Last-write-wins drift reconciliation (PLAN.md §7.3). Lamport-style logical clock, NOT wall-clock
// time: bumped by +1 on a local-origin mutation (this bar applied a delta/toggle itself — a new
// fact), or assigned outright to a peer's version when adopting its state (not a new fact, just
// aligning to one that already exists). Getting those two cases backwards — incrementing on
// adopt too — would make two bars that just converged each think they're now newest, oscillating
// forever instead of settling. See setBrightness() and peerStateWins().
//
// RAM-only, deliberately NOT persisted to NVS: every boot starts at 0 so this bar has zero standing
// in peerStateWins() until it hears otherwise. That's intentional — see the boot-query comment in
// setup(). A bar that remembered its last version across reboots could out-rank a peer's genuinely
// current state after being power-cycled mid-session (e.g. after a firmware update), stomping the
// group with a stale level instead of joining it.
uint32_t brightnessVersion = 0;
uint32_t lastHeartbeatMs = 0;
uint8_t selfMac[6];

// Rate-limited NVS persistence (PLAN.md §6)
bool nvsDirty = false;
uint32_t lastChangeMs = 0;

// User LED state machine (PLAN.md §8)
enum UserLedMode { LED_BOOT_WAIT, LED_NORMAL, LED_FLASH, LED_PAIRING, LED_FAULT };
UserLedMode userLedMode = LED_BOOT_WAIT;
uint32_t userLedFlashUntilMs = 0;

// Recv callback runs in the Wi-Fi task context — keep it to a copy into this small ring buffer,
// do real handling in loop() (PLAN.md §6). Sender MAC is carried alongside the packet (from
// esp_now_recv_info_t::src_addr, not the wire payload) for peerStateWins()'s tie-break.
// Widened from 8: this queue is shared by every incoming packet type, including real
// MSG_DELTA/MSG_TOGGLE from controllers, and STATE_HEARTBEAT_MS (config.h) is fast enough now
// that a burst of peer heartbeats could otherwise fill it right as a real command arrives — the
// recv callback drops silently on overflow, so headroom here matters more than it used to.
#define RX_QUEUE_LEN 16
struct RxEntry {
	LinkLitePacket pkt;
	uint8_t srcMac[6];
};
RxEntry rxQueue[RX_QUEUE_LEN];
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

void setBrightness(uint8_t pct, uint32_t version);
void applyDelta(int8_t delta);
void applyToggle();
void sendPacket(MsgType type, int8_t delta, uint8_t level);
bool peerStateWins(uint32_t peerVersion, uint8_t peerLevel, const uint8_t *peerMac);
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

	// Step 2 (PLAN.md §5): load persisted state, drive output immediately so the bar isn't dark
	// while radio comes up. `pct` is trusted only as a *display placeholder* here, not as this
	// bar's claim on the group's true state — it's shown immediately, then version 0 (below) makes
	// sure it's the first thing given up the moment a peer answers the boot query in setup(). If no
	// peer answers (solo bar, or first one up), nothing ever beats it and it quietly becomes the
	// group's starting value — which is correct, since in that case it's the only value that exists.
	//
	// `groupId`, by contrast, IS trusted outright and unconditionally: it's not a brightness claim
	// but this bar's own identity/broadcast-domain membership, and every packet's groupId is
	// checked against it before anything else runs (handleRxQueue()). There's no query/converge
	// step for it the way there is for brightness — whatever NVS says this bar's group is, is final
	// until a future pairing flow (PLAN.md §9, not implemented) explicitly changes it. Note this is
	// a filter, not authentication — ESP-NOW is unencrypted (PLAN.md §10), so it only isolates
	// installations from each other, it doesn't stop a bad actor in radio range from spoofing it.
	prefs.begin("linklite", false);
	uint8_t loadedPct = prefs.getUChar("pct", BRIGHTNESS_DEFAULT_PCT);
	groupId = prefs.getUChar("grp", DEFAULT_GROUP_ID);
	lastNonZeroPct = loadedPct > 0 ? loadedPct : BRIGHTNESS_DEFAULT_PCT;
	setBrightness(loadedPct, 0); // version 0: no standing until a peer says otherwise, see setup()'s boot query below

	// Step 3 (PLAN.md §5): Wi-Fi STA pinned to a fixed channel, ESP-NOW up.
	WiFi.mode(WIFI_STA);
	WiFi.disconnect();
	// Same reasoning as linklite-controller's identical call: Wi-Fi power-save (modem sleep, DTIM
	// wake, etc.) is meant to coordinate with an AP connection we don't have here. Unlike the
	// controller, the bar is never told to sleep — it's a passive ESP-NOW listener sitting idle
	// between its own 15s heartbeats (STATE_HEARTBEAT_MS) — which is exactly the condition under
	// which the default power-save mode (WIFI_PS_MIN_MODEM) is most likely to start missing RX
	// windows, worsening the longer the radio goes without our own TX activity. Disabling it keeps
	// the radio fully awake at the cost of some extra idle current draw, which the bar (mains-fed
	// via USB-C PD, unlike the battery-powered controller) can afford.
	esp_wifi_set_ps(WIFI_PS_NONE);
	// This is the radio's actual operating channel — trusted outright from config.h, never learned
	// or negotiated. ESP-NOW requires sender and receiver to be on the same channel at the hardware
	// level; there's no scan/handshake here, both sketches just hardcode WIFI_CHANNEL and assume it
	// matches (PLAN.md §5 step 3 / §9 file-layout note flags this as a manually-kept invariant since
	// the two sketches don't share a header yet). If one sketch's config.h drifts from the other's,
	// the result is total, silent radio-layer deafness — no error, no log, packets just never arrive
	// — because they're each doing the same thing to themselves that they'd do to a stranger: an
	// ESP-NOW node on the wrong channel isn't rejected, it's simply never heard from.
	esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
	WiFi.macAddress(selfMac); // cached once for peerStateWins()'s tie-break

	if (esp_now_init() != ESP_OK) {
		Serial.println("ESP-NOW init failed");
		userLedMode = LED_FAULT;
	} else {
		esp_now_peer_info_t peer = {};
		memcpy(peer.peer_addr, BROADCAST_MAC, 6);
		// Separate trust point from esp_wifi_set_channel() above, easy to lose track of: this is the
		// ESP-NOW *peer table's* channel field, not the radio's channel. Per Espressif's docs, 0
		// here means "whatever the interface's current channel is"; a nonzero value (as used here)
		// tells the ESP-NOW layer this specific peer is expected to be reachable on that channel
		// specifically, independent of what esp_wifi_set_channel() set. They're set to the same
		// WIFI_CHANNEL constant here so the two never disagree, but they're two different pieces of
		// state that happen to be kept in lockstep by convention, not by any enforced coupling — an
		// edit to one without the other is a compile-clean, silent way to reintroduce channel drift.
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
		// Any MSG_STATE replies received during the window already went through the same
		// peerStateWins() comparison as steady-state heartbeats (handleRxQueue()) — whichever had
		// the highest version won, including possibly our own persisted state if no peer beat it.
	}

	// PLAN.md §7.3: periodic MSG_STATE heartbeat so a bar that missed a delta converges instead of
	// drifting forever. Reconciliation on receipt happens in handleRxQueue()'s MSG_STATE case.
	if (millis() - lastHeartbeatMs >= STATE_HEARTBEAT_MS) {
		lastHeartbeatMs = millis();
		sendPacket(MSG_STATE, 0, brightnessPct);
	}

	persistIfDirty();
	updateUserLed();
}

// `version` is always supplied by the caller, never computed here — setBrightness() can't tell on
// its own whether this is a local-origin mutation (should increment brightnessVersion) or adopting
// a peer's state (should assign it outright). See the brightnessVersion comment up top.
void setBrightness(uint8_t pct, uint32_t version) {
	if (pct > BRIGHTNESS_MAX_PCT) pct = BRIGHTNESS_MAX_PCT;
	brightnessPct = pct;
	brightnessVersion = version;
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
	setBrightness((uint8_t)next, brightnessVersion + 1); // local-origin mutation: new logical event
	flashActivity();
}

void applyToggle() {
	if (brightnessPct > 0) {
		setBrightness(0, brightnessVersion + 1);
	} else {
		setBrightness(lastNonZeroPct, brightnessVersion + 1);
	}
	flashActivity();
}

// Deterministic total order for the rare case of two bars landing on the same logical version with
// different levels (e.g. a bar that was locally toggled while partitioned, and its own increment
// from a stale baseline happens to numerically match a peer's independently-advanced version).
// Under normal operation (every mutation is broadcast and applied identically everywhere) this
// never triggers — versions only tie with differing levels via a genuine concurrent/partitioned
// write, not the common "peer is just behind" case, which peerVersion > brightnessVersion covers.
// Arbitrary but consistent: larger MAC wins, so every bar comparing the same pair agrees.
bool peerStateWins(uint32_t peerVersion, uint8_t peerLevel, const uint8_t *peerMac) {
	if (peerVersion > brightnessVersion) return true;
	if (peerVersion == brightnessVersion && peerLevel != brightnessPct) {
		return memcmp(peerMac, selfMac, 6) > 0;
	}
	return false;
}

void sendPacket(MsgType type, int8_t delta, uint8_t level) {
	LinkLitePacket pkt = {};
	pkt.groupId = groupId;
	pkt.msgType = type;
	pkt.senderType = SENDER_BAR;
	pkt.seq = txSeq++;
	pkt.delta = delta;
	pkt.level = level;
	pkt.stateVersion = brightnessVersion; // meaningful for MSG_STATE only; harmless otherwise
	esp_now_send(BROADCAST_MAC, (uint8_t *)&pkt, sizeof(pkt));
}

// Runs in Wi-Fi task context — must stay short and non-blocking (PLAN.md §6).
void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
	if (len != sizeof(LinkLitePacket)) return;
	uint8_t nextTail = (rxTail + 1) % RX_QUEUE_LEN;
	if (nextTail == rxHead) return; // queue full, drop — better than blocking the Wi-Fi task
	memcpy(&rxQueue[rxTail].pkt, data, sizeof(LinkLitePacket));
	memcpy(rxQueue[rxTail].srcMac, info->src_addr, 6);
	rxTail = nextTail;
}

void handleRxQueue() {
	while (rxHead != rxTail) {
		LinkLitePacket pkt = rxQueue[rxHead].pkt;
		uint8_t srcMac[6];
		memcpy(srcMac, rxQueue[rxHead].srcMac, 6);
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
				// Last-write-wins reconciliation (PLAN.md §7.3): the same comparison handles both
				// boot-query replies and steady-state heartbeats — a strictly newer peer always
				// wins, an equal-version-but-different-level peer is resolved by MAC tie-break, and
				// anything older/losing is ignored. Replaces the old "first boot reply wins" rule,
				// which could stomp a correct persisted state with a lagging peer's stale reply.
				if (peerStateWins(pkt.stateVersion, pkt.level, srcMac)) {
					setBrightness(pkt.level, pkt.stateVersion);
					flashActivity();
				}
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
		// `brightnessVersion` is deliberately not persisted here — see its declaration up top. Only
		// `pct` (a display fallback, not a claim) and `grp` (this bar's identity) survive a reboot.
		prefs.putUChar("pct", brightnessPct);
		prefs.putUChar("grp", groupId);
		nvsDirty = false;
	}
}
