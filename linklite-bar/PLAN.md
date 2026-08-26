# linklite-bar — Firmware Plan

Target: **ESP32-C6-MINI-1**, FQBN `esp32:esp32:esp32c6`. This is the authoritative brightness
node — see `CLAUDE.md` at the repo root for hardware pinout, the sync philosophy, and
conventions. This document is the detailed design for the `linklite-bar` sketch; the
accompanying `linklite-bar.ino` is a skeleton that implements the settled parts and stubs the
open ones with `// TODO(plan §n)` markers back into this file.

## 1. Responsibilities

The bar is the **stateful** half of the system (see CLAUDE.md "Sync model"). It:

1. Owns the authoritative brightness value for itself, and converges that value with peer bars.
2. Drives the LED array via LEDC PWM with a gamma-corrected curve.
3. Accepts brightness deltas and toggle events from controllers over ESP-NOW.
4. Answers late-joining bars' state queries so they converge instead of resetting the group.
5. Persists last brightness across power cycles.

Controllers never hold state — they only emit intent (`CLAUDE.md` §Sync model). Any logic that
looks like "wait for the controller to tell me the absolute level" is wrong; the bar decides.

## 2. Brightness domain

- Internal representation: **`uint8_t percent`, 0–100**, clamped on every mutation. Percent (not
  raw duty) is what gets stored in NVS, sent as `MSG_STATE`, and reasoned about in logic — it's
  radio- and hardware-independent.
- Conversion to PWM duty happens **only** at the output stage (§4), via a gamma LUT.
- One encoder detent from the controller = **±2 percent** (`STEP_SIZE_PERCENT`, shared constant
  with `linklite-controller`). Chosen so a full 0→100 sweep is ~50 detents — adjust after bench
  testing feels right; it's the one "magic number" both sketches must agree on today since there's
  no shared header between sketches yet (see §9 Conventions & gotchas).

## 3. GPIO & peripherals

Per `CLAUDE.md`:

| Function      | GPIO | Mode                          |
|---------------|------|--------------------------------|
| User button 1 | 20   | `INPUT_PULLUP`, debounced      |
| User button 2 | 21   | `INPUT_PULLUP`, debounced      |
| User LED      | 22   | `OUTPUT`                       |
| Main LEDs     | 23   | LEDC PWM (drives MOSFET gate)  |

## 4. LED driver (LEDC)

- **API:** arduino-esp32 3.x's pin-based LEDC calls — `ledcAttach(LED_PIN, freq, resBits)` /
  `ledcWrite(LED_PIN, duty)` — not the old channel-number API.
- **Frequency:** 5 kHz. Well clear of flicker/audible range, comfortably inside what the AO3400A
  can switch cleanly.
- **Resolution:** 12-bit (0–4095) for smooth low-end dimming.
- **Gamma correction:** the MOSFET+LED array is a linear-duty light source; human perception is
  roughly a power curve. Map `percent` (0–100) → duty via a **precomputed 101-entry LUT**
  (`const uint16_t GAMMA_LUT[101] PROGMEM`), `duty = round(4095 * (percent/100)^2.2)`.
  **Do not call `pow()`/float math at runtime on every step** — the C6 is RISC-V with no hardware
  FPU, so `pow()` is a soft-float library call; fine once at table-generation time (or precomputed
  by a script and pasted in as constants), not fine in a hot path.
- **At 0%:** drive duty to exactly 0, not the LUT's rounded near-zero value, so "off" is really off.

## 5. Boot sequence

1. Init serial (debug only), GPIO modes, LEDC attach.
2. Load persisted `percent` from NVS (`Preferences`, namespace `"linklite"`); if absent, default
   to a safe **25%** rather than 0% or 100% (visible but not startling). Drive the LEDC output to
   this provisional value immediately — don't leave the bar dark while radio comes up.
3. Bring up Wi-Fi STA (no AP association) pinned to the fixed **`WIFI_CHANNEL`** (both sketches
   hardcode the same channel — ESP-NOW requires it, and an unassociated STA otherwise reuses
   whatever channel is cached in Wi-Fi NVS, which can silently drift between devices). Init
   ESP-NOW, register the broadcast peer (`FF:FF:FF:FF:FF:FF`) and the recv callback.
4. Broadcast `MSG_QUERY`. Wait up to **500 ms** for `MSG_STATE` replies.
   - Every reply received in the window goes through the same last-write-wins comparison as
     steady-state heartbeats (§7.3): whichever `(level, stateVersion)` has the highest version
     wins, including possibly this bar's own NVS-loaded state if no peer beats it. This replaced
     an earlier "first reply wins" rule, which could stomp a correct persisted state with a
     lagging peer's stale reply.
   - If none arrive: this bar *is* the group (first one up, or alone) — keep the NVS/default value.
5. Enter main loop.

## 6. Main loop

Cooperative, non-blocking — no `delay()` in `loop()`:

- Poll button 1 / button 2 with debounce (simple time-based debounce is enough; these aren't
  latency-critical like the encoder).
- Drain a small ring buffer of received ESP-NOW packets (the recv callback runs in the Wi-Fi task
  context — keep it to "copy packet into buffer," do the real handling in `loop()`).
- Apply pending deltas → update `percent` → clamp 0–100 → `ledcWrite` via gamma LUT → mark NVS
  "dirty."
- Rate-limited NVS persistence: only write after **2 s of quiescence** since the last change, not
  on every step. Flash write cycles are finite and a fast encoder spin can generate many deltas/sec.
- Update the user LED per the status table in §8.
- Broadcast a `MSG_STATE` heartbeat every `STATE_HEARTBEAT_MS` (~100 ms) — the primary bar-to-bar
  sync mechanism, not just rare-drift reconciliation (§7.3); affordable because a converged bar's
  `peerStateWins()` check on receipt is cheap when `(version, level)` already match.

## 7. Wire protocol (ESP-NOW)

One packed struct, identical in both sketches until they're pulled into a shared header:

```c
typedef enum : uint8_t {
  MSG_DELTA  = 0x01,  // relative brightness step, from a controller (or another bar, unused today)
  MSG_TOGGLE = 0x02,  // on/off toggle, from a controller
  MSG_QUERY  = 0x03,  // "what's the current level?" — sent by a bar on boot
  MSG_STATE  = 0x04,  // response to MSG_QUERY, or periodic heartbeat (see §7.3)
} MsgType;

typedef struct __attribute__((packed)) {
  uint8_t  groupId;      // isolates separate physical installations sharing radio range; 0 = default
  uint8_t  msgType;      // MsgType
  uint8_t  senderType;   // 0 = bar, 1 = controller
  uint32_t seq;          // sender-local monotonic counter; debugging/ordering only, not for dedup
  int8_t   delta;        // MSG_DELTA/MSG_TOGGLE payload, in percent points
  uint8_t  level;        // MSG_STATE payload, absolute percent (0-100) — bootstrap/reconcile only
  uint32_t stateVersion; // MSG_STATE payload: Lamport-style logical clock, see §7.3
} LinkLitePacket; // 13 bytes
```

**Important distinction:** `level` in `MSG_STATE` is *not* a "set to X" control command — CLAUDE.md
is explicit that absolute set-points must never drive the control plane, because they let two
racing controllers stomp each other. `MSG_STATE` exists only to bootstrap a bar that has no local
state to converge from (§5 step 4) and, optionally, to nudge quiescent drift (§7.3). A bar must
never apply a received `MSG_STATE.level` while it has an active/recent delta stream in flight.

### 7.1 Delta application

On `MSG_DELTA`: `percent = clamp(percent + pkt.delta, 0, 100)`. On `MSG_TOGGLE`: if `percent > 0`,
remember it as `lastNonZeroPercent` and set `percent = 0`; if `percent == 0`, restore
`lastNonZeroPercent` (or the 25% default if none recorded yet).

### 7.2 No relay/rebroadcast in v1

Bars do **not** rebroadcast deltas they receive. ESP-NOW broadcast is single-hop and, for the
target install size (a handful of bars in one room, well within radio range of every controller
and each other), every bar hears every controller directly — a mesh relay adds storm-prevention
complexity (loop detection, TTLs, dedup) for no benefit at this scale. If a future install needs
bars spread across a range no single controller can reach, revisit this — it's a deliberate scope
cut, not an oversight.

### 7.3 Drift correction — last-write-wins via logical clock, implemented

Missed broadcasts (radio interference, a bar briefly out of range) cause silent drift between bars
since delta application isn't acknowledged — previously the only recovery was manually cranking
every bar to a rail (0% or 100%) so the independent-clamping property (§11) forced convergence.

**Design:** each bar keeps `brightnessVersion: uint32_t`, a Lamport-style logical clock (not
wall-clock time) tied 1:1 to its own `percent` mutations:

- **Local-origin mutation** (this bar applied a delta/toggle it received, or a local button press) →
  `version = version + 1`. This bar just created a new fact.
- **Remote-origin mutation** (adopting a peer's state) → `version = peerVersion`, assigned outright,
  *not* incremented. This bar isn't creating a new fact, it's aligning to one that already exists.
  Incrementing here too would make two bars that just converged each think they're now newest,
  oscillating forever instead of settling — this distinction is the crux of the whole scheme.
- Deliberately **not** persisted to NVS. Every reboot starts at `version = 0`, so a restarted bar
  has no standing in `peerStateWins()` and must join the group's current state via the boot query
  (§5 step 4) rather than asserting whatever it last knew — a bar that remembered its version
  across reboots could out-rank a peer's genuinely current state after a power cycle (e.g. mid
  firmware update) and stomp the group with a stale level. `percent` is still persisted, but only
  as a display fallback for the solo/no-peers case, not as a claim on the group's state.

Each bar broadcasts `MSG_STATE` (now carrying `level` **and** `stateVersion`) every
`STATE_HEARTBEAT_MS` (15 s). On receiving *any* `MSG_STATE` — heartbeat or boot-query reply, same
code path — a bar compares:

- `peerVersion > localVersion` → adopt the peer's `(level, version)`.
- `peerVersion == localVersion` but `level` differs → deterministic tie-break on MAC address
  (larger wins), so the group can't oscillate. This only arises from a genuinely concurrent or
  partitioned write (e.g. a bar toggled locally while out of range, whose increment from a stale
  baseline happens to numerically match a peer's independently-advanced version) — under normal
  operation, every mutation is broadcast and applied identically everywhere, so all bars advance
  their version in lockstep and this branch essentially never fires.
- `peerVersion < localVersion` → ignore; the peer is behind and will catch up on its next heartbeat.

This **replaced** the originally-proposed "nudge one step toward the received value if quiescent"
design. A bar only diverges when it's already missed updates — i.e. it's already visibly wrong —
so snapping straight to the winning value is the more direct fix, and the logical-clock ordering
already gives the "don't override a bar mid-adjustment" property the nudge/quiescence heuristic was
reaching for: a bar applying its own deltas keeps incrementing its own version, so a stale peer
heartbeat can't be `>` it and can't win.

Bench-validated behavior to confirm: induce packet loss on one bar mid-session, confirm it
re-converges within one heartbeat interval once back in range, and confirm no oscillation between
two bars deliberately desynced to the same version with different levels.

## 8. User LED (GPIO22) status codes

| State                              | Pattern                        |
|-------------------------------------|---------------------------------|
| Normal / converged                  | Solid on                        |
| Booting, waiting for `MSG_STATE`    | Slow blink (2 Hz) until step 5  |
| Delta received (activity indicator) | Brief single flash (~30 ms)     |
| Pairing / group-assign mode (§9)    | Fast blink (5 Hz)               |
| Fault (e.g. ESP-NOW init failed)    | Triple-flash, repeating         |

## 9. Buttons 1 & 2 — OPEN DECISION, proposed defaults

CLAUDE.md doesn't define these; per the project's own convention of flagging rather than silently
deciding (see its Wireless transport section), here's a proposal to confirm, not a final answer:

- **Button 1, short press:** local on/off toggle — same effect as a controller `MSG_TOGGLE`,
  applied locally and then broadcast so peers follow. Useful for a bar with no controller nearby.
- **Button 1, long press (~2 s):** identify — fast-blink the user LED and pulse the main LEDs
  briefly, useful for figuring out which physical bar you're touching during setup.
- **Button 2:** reserved for **group/pairing management** (`groupId` assignment) — mechanism TBD.
  The simplest version: long-press enters pairing mode (fast user-LED blink), during which the bar
  accepts a `groupId` broadcast from a designated "pairing controller" gesture (e.g. a long
  controller-button-press while in range). This needs a controller-side counterpart that doesn't
  exist yet — out of scope until `linklite-controller`'s button role is revisited past its current
  on/off-toggle default (see repo root `CLAUDE.md`).

The skeleton wires both buttons with debounce and stubs their handlers; button 1's short-press
toggle is implemented (it's simple and low-risk), everything else is `// TODO(plan §9)`.

## 10. Persistence (NVS via `Preferences`)

Namespace `"linklite"`, keys: `pct` (`uint8_t`, last brightness — display fallback only, not a
state claim) and `grp` (`uint8_t`, `groupId`, default 0). `brightnessVersion` is intentionally
**not** persisted (see §7.3) so every boot re-joins the group via query instead of asserting a
possibly-stale version. Write path is rate-limited per §6. No encryption/auth on the ESP-NOW link
in v1 — flag
per CLAUDE.md's "don't silently pick" spirit: this means anything in radio range can inject deltas.
Acceptable for a single-room consumer light; revisit if the threat model changes (`esp_now` supports
per-peer AES-CCM encryption if needed later).

## 11. Edge cases

- **Two controllers turning at once:** deltas are commutative by construction (§7.1) — no special
  handling needed, this is the whole point of the delta model.
- **Clamping at rails:** every node clamps independently to 0–100; since clamping is deterministic
  and the same function everywhere, nodes can't drift apart at the rails even without coordination.
- **Late join:** handled by boot-time `MSG_QUERY`/`MSG_STATE` (§5). A bar joining mid-session must
  not default to a guessed value if peers are present.
- **Solo bar (no peers, no controller):** boots to NVS value or 25% default; buttons still work
  locally.
- **NVS write during brownout/power loss:** rate-limiting (§6) minimizes the window; worst case is
  losing up to 2 s of the most recent change, not corruption (`Preferences` commits are atomic per
  key).

## 12. File layout

- `linklite-bar.ino` — `setup()`/`loop()`, wires everything below together.
- `config.h` — pin defs, `LinkLitePacket`/`MsgType`, `WIFI_CHANNEL`, `STEP_SIZE_PERCENT`,
  `GAMMA_LUT`, timing constants. Kept as a header (not split .cpp/.h per module) since the sketch
  is still small; revisit if it grows past a few hundred lines per file.

## 13. Bench test plan (before calling this done)

1. Single bar, no controller: boots to default, buttons work, NVS persists across power cycle.
2. Single bar + single controller: rotate CW/CCW, confirm smooth gamma-correct dimming; push
   button toggles; confirm `STEP_SIZE_PERCENT` feels right, tune if not.
3. Two bars, one controller: both converge to the same brightness on every delta; power-cycle one
   bar mid-session and confirm it re-queries and converges instead of resetting.
4. Two bars, two controllers, simultaneous opposite turns: confirm net delta lands correctly with
   no fighting/flicker.
5. Induce packet loss (walk a bar out of range briefly): confirm behavior is "graceful drift," not
   a crash or LEDC glitch; use this to validate (or invalidate) §7.3 before implementing it.

## 14. Explicit open items (don't silently resolve — confirm first)

- §7.3 Drift-reconciliation heartbeat — implemented (last-write-wins via logical clock); not yet
  bench-validated against induced packet loss. `STATE_HEARTBEAT_MS` (~100 ms) chosen to make the
  heartbeat the primary sync path rather than a rare-case backstop; `RX_QUEUE_LEN` (linklite-bar.ino)
  was widened alongside it since the queue is shared with real controller commands.
- §9 Button roles — button 1 short-press implemented as proposed; everything else pending.
- §9 groupId assignment/pairing mechanism — no controller-side counterpart exists yet.
- `STEP_SIZE_PERCENT` (2%/detent) and the 25% power-on default — reasonable starting guesses, tune
  after bench testing.
- No ESP-NOW encryption — acceptable for now, flagged for revisit.
