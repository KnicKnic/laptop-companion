# notes around sleep

I was trying to figure out how to get auto light sleep to get fixed. Had issues with dropping connection within first few minutes. Turns out auto light sleep & SMP are incompatibile


Fixed by enabling single core


Need to still investigate 2 options
commented out - CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP (need to figure out if this is necessary, also added CONFIG_ESP_SLEEP_PSRAM_LEAKAGE_WORKAROUND )

CONFIG_ESP_SLEEP_PSRAM_LEAKAGE_WORKAROUND - I don't think I am using hte psRAM I could shut it down
CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP - I think this takes a few KB of ram to save CPU info, also costs wakeup time


## TODO
still have roughly 1/3 wakeup of RTOS... need to get down significantly.

I wonder if this is any of the power management locks data. (now that I have accurate sleep tracking for locks I wonder if I can turn off all pm stats)

Could also be other interrupts and information.

Also need to mess with 
  CONFIG_FREERTOS_EXPECTED_IDLE_TIME_BEFORE_SLEEP=3

## BLE wake timing on the S3 (x4pro)

### How BT wakes the CPU out of auto light sleep (IDF v5.5 `bt/controller/esp32c3/bt.c`, shared by C3 and S3)
* Before each controller sleep, bt.c arms an esp_timer named `btSlp` for `us_to_sleep - uncertainty`.
  When it fires, the controller task takes the `bt` PM lock (APB max) so the CPU is awake before the
  controller (timed by main XTAL, `CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL` + `MAIN_XTAL_PU_DURING_LIGHT_SLEEP`) wakes.
* `uncertainty = max(us_to_sleep / 2048, 1800us)`, **or 7% of the sleep if the RTC slow clock is RC_SLOW**
  (`BTDM_RTC_SLOW_CLK_RC_DRIFT_PERCENT`). That 7% is the "can't beat 93% idle" problem from the X3;
  `CONFIG_RTC_CLK_SRC_INT_8MD256=y` avoids it and x4pro already uses it.
* The CPU's light-sleep wake is timed by the RTC slow clock. With PM enabled IDF re-calibrates it on every
  light sleep (`CONFIG_PM_LIGHTSLEEP_RTC_OSC_CAL_INTERVAL=1`) using only 10 slow-clock cycles
  (`RTC_CLK_SRC_CAL_CYCLES` in sleep_modes.c). `CONFIG_RTC_CLK_CAL_CYCLES=3000` only affects the boot calibration.
* On the S3 the light-sleep exit overhead is much larger than the C3 (IDF default 8628us vs 105us when
  `CONFIG_PM_SLP_IRAM_OPT` is off; IDF measures it once after the first wake).
* S3 USB-Serial-JTAG does not survive light sleep (pads are disabled every sleep), so serial logs are not
  usable while auto light sleep is on. Use the BLE diagnostics characteristic instead (below).

### Config fix
`CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP` defaults to `y`, so commenting it out in platformio.ini did **not**
disable it (the generated sdkconfig.x4pro still had it on). It is now explicitly unset.
The light-sleep code is also kept in IRAM (`CONFIG_PM_SLP_IRAM_OPT`, `CONFIG_PM_RTOS_IDLE_OPT`), which cut
late BT wakes ~5x in testing.

### Instrumentation (`src/BtSleepTiming.*`, linker `--wrap` of esp_timer_create/start_once/stop and rtc_clk_cal)
* **BTW line** (power stats page, serial, BLE): for every `btSlp`-scheduled wake, the margin between the CPU
  taking the `bt` lock and the controller's wake. `late` = margin < 0, `miss` = margin < -2.5ms (past the modem
  wake-up delay, connection event very likely missed). `lead` = timer lead used / IDF default. `bt%` = time the
  bt lock was held.
* **RTC line**: every 30s a 1024-cycle reference calibration is compared to each per-sleep 10-cycle calibration
  (`raw min..max ppm`). Wake error ~= ppm x sleep length, and must stay inside the BT lead. ` RC` would mean the
  7% path is active.
* Both counters can be reset over BLE (`reset=1`).

### Experimental fixes (off by default; settings page rows or `companion_settings.json` `power_management`)
* `bt_wake_autotune`: replaces IDF's fixed lead with one auto-calibrated from the measured margins
  (target 400us + measured slow-clock error x sleep length, +1ms on any late wake, slow step down).
* `rtc_cal_filter`: feeds IDF the periodic 1024-cycle reference calibration instead of each 10-cycle per-sleep
  sample (measured biased by ~+835 ppm, worst 963 ppm, on the X4 Pro).

### Testing
1. Flash: `pio run -e x4pro -t upload` (set `PLATFORMIO_BUILD_DIR` to a short path such as `C:\pb` if the
   worktree path is long; libsodium objects otherwise exceed Windows MAX_PATH).
2. Stop laptop_host (or keep it and pass `--no-handshake`), then run the soak test from the repo root:
   `dotnet run --project ble_sleep_soak -- --minutes 60 --set "pm=1 als=1"`
   It connects like laptop_host, reads the `...0000f0` diagnostics characteristic every 30s, logs disconnects,
   and writes a `ble_sleep_soak_*.log`. The `pm/als/tune/rtcf` commands apply live and are not saved to SD.
3. A/B an option: `--set "pm=1 als=1" --ab tune --phase-minutes 20` alternates tune=0/1 and resets the device
   counters at each phase; the per-phase summary at the end compares sleep %, bt awake %, late/miss and disconnects.
4. What to look for: `miss`/`late` > 0 or `sto` (supervision timeouts, reason 0x208) means the CPU is waking too late;
   `cal` ppm x sleep length approaching the lead means slow-clock calibration noise is eating the margin.
5. Safety (the device must never be left unreachable):
   * Light sleep is blocked for 60s after every boot (`boot-usb` PM lock), so USB/flashing always works right
     after a reset.
   * Light sleep enabled over BLE (`als=1`) is not saved to SD (a reboot reverts it), is reverted after 3
     device-side disconnects or 2 minutes without a link, and has a 90s dead-man timer that only the soak tool's
     `ka=1` keepalives (sent after every successful read) extend.
   * While light sleep is on, USB is dead (no flashing, no serial); send `--set "als=0"` or reset the device.
     Opening COM3 with pyserial's default DTR/RTS resets the chip; set dtr/rts False before `open()`.
   * If Windows still holds the connection the device does not advertise; use `--address 7CE8B1AB5641`.
   * Trust the device-side `gap_disc`; Windows' ConnectionStatus events flap and over-count.

### Results (X4 Pro, idle conn 120ms / latency 10)
| Config | Result |
|---|---|
| PM on, light sleep off | stable; bt lock ~1-4%; min BT wake margin ~0.9-1.0ms of the 1.8ms lead |
| light sleep, CPU power-down on (as previously shipped) | link drops every ~17s; 22 drops in 3 min |
| light sleep, CPU power-down off | still drops: 414 of 1878 BT wakes late, 9 past -2.5ms, min margin -3.2ms, supervision timeout |
| + sleep code in IRAM (`PM_SLP_IRAM_OPT`, `PM_RTOS_IDLE_OPT`) | fewer late wakes (79 of 1779) but 15 past -2.5ms, min -4.8ms, 2 supervision timeouts |
| + `rtcf=1` | one panic (rst=4) right after enabling, not reproduced; retry: 1543 sleeps, still a supervision timeout |
| + `tune=1` (laptop fan moved away) | **stable**: 0 disconnects in 12 + 25 min; ~97% light sleep, bt lock 1.1%; lead converged to ~5-6ms; 7-9 late wakes, all during convergence |
| control, same conditions, `tune=0` | 1015 of 2086 wakes late, 21 missed, supervision timeout; dead-man switch turned light sleep off by itself |

* Slow clock is 8MD256 at ~71 kHz, so the 7% RC_SLOW path is not active.
* IDF's 10-cycle per-sleep calibration of it is badly off on the S3: +947..+2189 ppm vs the 1024-cycle reference
  (EMA bias +835..+1885 ppm, worst seen -2272 ppm). At a 1.2s BT sleep that alone is 1-2.6ms, more than the
  1.8ms lead. A likely contributor is RC_FAST shifting with the digital supply voltage, which differs between
  calibration (awake, DFS) and light sleep; that would also explain why a better awake calibration (`rtcf`)
  did not fully fix it.
* The BT controller's sleep timer runs on the main XTAL, so a late wake shows up directly as a negative margin.
  That is what `tune=1` closes the loop on (+1ms per late wake, then converges to 400us + calibration guard).
* Twice the device ended up unreachable (no advertising, no USB, no ROM bootloader) after a failing light-sleep
  run. Cause unknown: deep sleep from a glitched power-button read after a light-sleep wake (power press is a
  single unconfirmed `digitalRead` edge in X4ProInputManager) or a hang. The next boot's `rst=` tells:
  8 = woke from deep sleep, 4 = panic, 1 = power-on/reset.

### Conclusion
`bt_wake_autotune` (now on by default) is what makes auto light sleep + BLE work on the S3: the X3 trick of
avoiding the 7% RC window was necessary but not sufficient, because the 8MD256 calibration is still off by
hundreds to thousands of ppm, more than IDF's fixed 1.8ms lead covers at 1.2-1.3s BT sleeps. The tuner
trades ~4ms more awake per BT wake (~0.3% duty) for a stable link. Keep the laptop fan off the device when
measuring: the warm air roughly doubled the calibration error.

To use it day to day, turn on "Auto light sleep" on the Settings page (saved to SD). USB stays usable for
60s after each boot; after that, reset the device to flash.

### Possible follow-ups
* Lower the tuner's target (400us + guard, +1ms per late wake) once longer soaks show margin to spare.
* Check `rtcf=1` together with `tune=1`; with the fan removed the 10-cycle calibration was only -600..+1000 ppm.
* Debounce the X4 Pro power button (single `digitalRead` edge) in case a wake glitch caused the earlier
  "device unreachable" episodes.