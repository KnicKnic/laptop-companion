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
* IDF's assumed light-sleep exit overhead (`DEFAULT_SLEEP_OUT_OVERHEAD_US`) is much larger on the S3:
  8628us when `CONFIG_PM_SLP_IRAM_OPT` is off and 382us when it is on, vs 105us on the C3. It is only the
  starting estimate: after the first real wake IDF measures the exit time and uses that instead.
  Source: ESP-IDF v5.5 `components/esp_hw_support/sleep_modes.c` (in pioarduino's
  `framework-espidf` v5.5.2: defaults at lines 172-180, measurement at lines 1473 and 1652-1653;
  [upstream](https://github.com/espressif/esp-idf/blob/release/v5.5/components/esp_hw_support/sleep_modes.c)).
  IDF wakes early by this amount, so it shifts the wake, it does not make it late.
* S3 USB-Serial-JTAG does not survive light sleep (pads are disabled every sleep), so serial logs are not
  usable while auto light sleep is on. Use the BLE diagnostics characteristic instead (below).

### Config fix
`CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP` defaults to `y`, so commenting it out in platformio.ini did **not**
disable it (the generated sdkconfig.x4pro still had it on). It is now explicitly unset.
The light-sleep code is also kept in IRAM (`CONFIG_PM_SLP_IRAM_OPT`, `CONFIG_PM_RTOS_IDLE_OPT`).

**pioarduino ignores linker-fragment options.** With `custom_sdkconfig`, pioarduino rebuilds the IDF libs and
copies the `.a` files and `memory.ld` into framework-arduinoespressif32-libs, but not the generated
`sections.ld`, so the Arduino link keeps the prebuilt one: `esp_light_sleep_start`, `vApplicationSleep`,
`esp_pm_impl_waiti` stayed in flash even with the IRAM options on (verified with `nm`). Every Kconfig option
implemented through linker fragments was silently ignored. `scripts/install_idf_sections_ld.py` (post: script on
the x4pro env) installs the generated `sections.ld` during the IDF phase. To force that phase after changing the
script, change `custom_sdkconfig` or the `# TASMOTA__<hash>` first line of sdkconfig.defaults.

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
| + sleep code in IRAM (`PM_SLP_IRAM_OPT`, `PM_RTOS_IDLE_OPT`); options were not actually applied yet, see "pioarduino ignores linker-fragment options" | fewer late wakes (79 of 1779) but 15 past -2.5ms, min -4.8ms, 2 supervision timeouts (difference likely run-to-run/temperature) |
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

### Where the wake delay comes from (btSlp due -> bt lock), `tune=1` unless noted, laptop moved away
`wl` = light-sleep exit vs timer due, `dsp` = due/exit -> btSlp callback (timer ISR + esp_timer task),
`off` = callback -> bt lock (controller task). avg / max in us.

| Config | wl | dsp | off | late | tuned lead | light sleep |
|---|---|---|---|---|---|---|
| IRAM opts not applied, 10 MHz floor | - | - | - | 7-9 | 5-6 ms | 75-88% |
| IRAM applied, 10 MHz floor | -866 / +417 | **1230 / 2704** | 17 / 48 | 2 | 3.9 ms | 88.2% |
| IRAM applied, 40 MHz floor | -1224 / +20 | **228 / 712** | 18 / 42 | 0 | 2.0-2.2 ms | 91.7% |
| same, `tune=0` (IDF's 1.8 ms) | -770 / -16 | 236 / 711 | 20 / 43 | 0, min margin 0.75 ms | 1.8 ms | 93.0% |
| same, `tune=0`, PM-stats dump task off | -175 / +22 | 230 / 711 | 18 / 46 | 0, min margin 1.05 ms | 1.8 ms | 93.7% |

* The CPU wakes on time or early; the cost was dispatching the timer at the 10 MHz DFS floor (ISR, context
  switch, PLL ramp to 240 MHz all at 10 MHz). The DFS floor is now 40 MHz (XTAL, no divider), as in IDF's
  BLE power-save example. Live A/B over BLE: `minf=10|20|40|80`.
* The 15 s PM-stats dump task (formats PM/timer tables, writes to the dead USB console) costs ~0.3 ms of the
  tail. It is now off by default (the task blocks until enabled); `dump=1` turns it on live.
* The 8MD256 calibration error (-600..+1000 ppm here, ~1 ms at 1.3 s sleeps) is invisible to these margins
  (esp_timer is corrected with the same calibration). `tune=1` keeps a guard for it; with `tune=0` the link held
  for 14 minutes but has no margin for it.

### Conclusion
Auto light sleep + BLE works on the S3 once the light-sleep path really is in IRAM and the DFS floor is 40 MHz;
then IDF's stock 1.8 ms lead is enough in testing. `bt_wake_autotune` stays on by default as a safety net for
slow-clock calibration error (it settles ~0.2-0.4 ms above IDF's lead). The X3 trick of avoiding the 7% RC
window is still required. Keep warm air off the device when measuring: it roughly doubled the calibration error.

To use it day to day, turn on "Auto light sleep" on the Settings page (saved to SD). USB stays usable for
60s after each boot; after that, reset the device to flash. Do not run esptool while the device is light
sleeping: twice it left the device unreachable (no advertising, no ROM response) until a manual reset.

### Possible follow-ups
* Done: the PM-stats dump task is off by default (`dump=1` enables it). Still open: turn PM/esp_timer
  profiling (`CONFIG_PM_PROFILING`, `CONFIG_ESP_TIMER_PROFILING`) and the stats `--wrap` hooks off for normal use.
* Done: the companion BLE worker is event-driven. It sleeps until its next deadline (pending-button settle,
  participation pulse, handshake/responsive timeouts, 5 s advertising watchdog while disconnected) or until a
  BLE event wakes it, instead of polling every 500 ms. Sleep diagnostics are built once per connection and
  refreshed every 5 s only after a client writes the characteristic (ble_sleep_soak does on every read).
* Debounce the X4 Pro power button (single `digitalRead` edge) in case a wake glitch caused the earlier
  "device unreachable" episodes.
* Settings page: 11 rows fit with 16 px to spare on the 480x800 portrait layout and there is no scrolling;
  another row needs paging.