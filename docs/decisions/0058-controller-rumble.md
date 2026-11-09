# 0058 — Controller rumble and haptic feedback

<!-- decision
state: accepted
date: 2026-11-09
replaces: none
code: runtime/src/svc_hid.c, runtime/src/window.c, runtime/src/window.h, runtime/src/bindings.c, docs/debugging.md
-->

Status: accepted (2026-11-09). Translates guest Switch HD Rumble IPC commands to the host controller via SDL3.

## Context

On Nintendo Switch, games send vibration data through `nn::hid` IPC calls (`SendVibrationValues` cmd 206, and
`SendVibrationValue` cmd 201). Since the start of the port (decision 0014, commit `d47c8b3`), vibration was stubbed out
as `{206, SIMPLE_OK, 0, "SendVibrationValues(aruid, X: handles, X: values): dropped"}`: vibration requests were accepted
with success so the game wouldn't fail, but the vibration data was dropped and never forwarded to the connected host controller.

## Findings

- **IPC buffers**:
  `SendVibrationValues` provides two buffers:
  - `X[0]`: array of 4-byte `VibrationDeviceHandle` integers (specifying the target controller npad ID and left vs. right actuator).
  - `X[1]`: array of 16-byte `VibrationValue` structs:
    `{ float amp_low; float freq_low; float amp_high; float freq_high; }`.
- **Downmix to host dual-motor controllers**:
  Standard PC controllers (Xbox, DualShock/DualSense in standard rumble mode, Switch Pro over SDL) provide dual ERM motors:
  a heavy low-frequency motor and a lighter high-frequency motor, controlled by `SDL_RumbleGamepad(pad, low, high, duration_ms)`.
  The low-frequency band (`amp_low`) maps to the heavy motor, and the high-frequency band (`amp_high`) maps to the light motor.
  When multiple actuators are targeted for Player 1 (e.g. stereo left/right Joy-Con actuators), the maximum intensity across the
  actuators is computed for each motor:
  `low_u16 = (uint16_t)(max_low * 65535.0f)`, `high_u16 = (uint16_t)(max_high * 65535.0f)`.
- **Duration and cancellation**:
  While active, the game streams updates periodically; a 1000 ms duration maintains active rumble across update intervals.
  When vibration ceases, the game transmits a zero-amplitude update, which cancels rumble immediately (`duration = 0`). If
  updates stop abruptly without a zero packet, rumble automatically expires after 1000 ms.
- **Thread safety**:
  `SDL_RumbleGamepad` is documented by SDL3 as thread-safe to call from any thread. Access to the active `SDL_Gamepad*` pointer
  is synchronized using the existing `input_lock` in `runtime/src/window.c`.

## Decision

1. **`window_rumble`**:
   `runtime/src/window.c` implements `window_rumble(uint16_t low, uint16_t high, uint32_t duration_ms)`. If `gamepad` is active,
   it calls `SDL_RumbleGamepad`.
   - `YW4_RUMBLE=0` disables rumble.
   - `YW4_RUMBLE_GAIN=<float>` scales vibration strength (default 1.0).
2. **`svc_hid.c` dispatch**:
   `runtime/src/svc_hid.c` handles commands 201 and 206, parses the `VibrationValue` entries for Player 1, and routes them to
   `window_rumble`.
3. **Controls overlay toggle**:
   `runtime/src/bindings.c` adds a "Controller vibration" toggle row on the Controls page right under the mouse
   sensitivity rows (with the battle/explore set toggle positioned beneath it so it directly heads the key bindings
   table). In `runtime/src/gamemenu.c`, the toggle renders as a full coloured pill when active (`On`) and a border-only
   pill when inactive (`Off`). Toggling vibration on triggers a 1-second rumble burst (`window_rumble_test`), which is
   protected in `runtime/src/window.c` against premature cancellation by the guest engine's 60 Hz zero-amplitude idle
   vibration updates. Choices are saved to `settings.ini` (`YW4_RUMBLE=1` / `0`).

## Shaping for PC pads (2026-11-10)

Played as the game sends it, the rumble was weak on a DualSense: most effects are too faint to start a spinning
motor. Gemini proposed a minimum speed with a curve, a split of the low band by frequency, and the Xbox trigger
motors. Measured first (late-game save, `YW4_RUMBLE_LOG=1`, a few minutes of play):

- Effects are smooth swells of about 0.4 s (rise ~10 frames, fall ~13), often back to back. Amplitudes are mostly faint:
  median 0.04 (low band) and 0.07 (high band), peaks 0.12 and 0.6.
- **The frequencies carry no pitch.** The game scales all four values of a `VibrationValue` with the swell's envelope:
  frequency / amplitude stays constant through a swell (e.g. 12 Hz at 0.024 up to 150 Hz at 0.12, high band). The
  split by frequency was dropped.
- A minimum speed with a curve made it stronger, but the motors never stopped between swells (owner: "BUZZZ BUZZZ
  BUZZZ": strong, not sharp).

What `window_vibration` (`runtime/src/window.c`) does now, per motor (low band to the heavy motor, high band to the
light one), as force-feedback drivers do with such motors:

1. Below 0.04: off. This drops the faint texture and leaves a gap between back-to-back swells.
2. A swell's start: a 50 ms kick at 70% speed, so the motor spins up at once. This is the sharp edge.
3. While it holds: 0.10 + 0.90 × a^0.75.
4. While it falls: 55% of that, so the motor stops before the next swell.
5. Xbox One / Series pads also get the high band's kicks on their trigger motors (45%).

In the run after the change the motors stopped 46 times between swells and 54 swells got the kick. Owner
(DualSense): "better, good enough", more tuning after 1.0. Switches: `YW4_RUMBLE_SHAPE=0` (as sent, linear),
`YW4_RUMBLE_TRIGGERS=0`, `YW4_RUMBLE_LOG=1`.

Open: the kick is the same for every swell (its peak isn't known when it starts); a weak swell may feel as punchy as
a strong one. Xbox trigger motors are untested (no Xbox pad here). The DualSense's own haptics (audio-driven, USB
only) are not used: SDL's rumble emulation drives them as two motors.
