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
