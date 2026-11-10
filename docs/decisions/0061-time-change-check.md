# 0061 — The game's "time was changed" check is answered "not changed"
<!-- decision
state: accepted
date: 2026-11-11
replaces: none
code: runtime/src/svc_time.c, recomp/overrides.toml
-->

Status: accepted (2026-11-11). The owner saw "The time on the console has been changed. Changing the time
repeatedly will give you penalties such as not being able to crank the Crank-a-Kai!" on every load, without
having changed anything.

## What we found

The check is the game's function at `main+0x3bd260`. It makes a clock snapshot of now
(`nn::time::ClockSnapshot::CreateWithStandardSystemClock`), takes the snapshot kept in the save (its object
`+0x160`; `main+0x3bd6b0` stores a new one there) and answers "changed" when any of three things holds:

1. `nn::time::CalculateStandardUserSystemClockDifferenceByUser(now, saved)` is not 0. On a Switch this is how
   far the user moved the clock by hand in the settings: 0 when time merely passed.
2. The two snapshots' time zone names (`GetLocationName`) differ.
3. A CRC-32 of the console's pseudo device id (`nn::oe::GetPseudoDeviceId`, printed as a UUID string) differs
   from the one in the save (`+0x230`).

Two of them fired here through no doing of the player:

- Our time service (command 500) answered 1 with the time between the two snapshots, the real time since the
  save. Coming back the next day looked like pushing the clock forward a day.
- A save made on a Switch or in an emulator holds that device's time zone name and id. The late-game test save
  holds "Asia/Bangkok"; our service calls the zone "LOCAL" (logged by the hook).

## Decision

- Command 500 answers 0: a PC has no console clock for the user to set.
- A hook on `main+0x3bd260` (`yw4_hook_time_changed`) answers the whole check with "not changed" instead of
  running it, so a save brought over from a Switch or an emulator gives no warning and no penalty count for 2
  and 3 either. Its first call logs the save's time zone name and device id CRC.

The game still gets the real date and time (the snapshots' times, `StandardUserSystemClock::GetCurrentTime`),
so the time of day and daily resets follow the PC's clock. Changing the PC's clock is no longer noticed.

## How it was checked

The owner loaded the late-game save: with command 500 alone fixed the warning still showed (the time zone);
with the hook it is gone.

## Not done yet

- Whether a penalty is already counted in saves that saw the warning before, and where the game keeps it.
