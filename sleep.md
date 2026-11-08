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
* `rtc_cal_filter`: feeds IDF an EMA (1/16) of the 10-cycle per-sleep calibrations instead of each noisy sample.

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
5. Recovery: while auto light sleep is on, USB is dead (no flashing, no serial). Send `als=0` over BLE
   (`--set "als=0"`), or power-cycle; the BLE commands are not saved, so a reboot reverts to the SD settings.
   Opening COM3 with pyserial toggles DTR/RTS and resets the chip.

### Results so far (X4 Pro, idle conn 120ms / latency 10)
* Slow clock measured ~70.9 kHz (8MD256, not RC), so the 7% path is not active. The boot 3000-cycle
  calibration was already ~1239 ppm off the 1024-cycle reference a few minutes after boot (RC_FAST drifts).
* PM on, light sleep off: link stable, bt lock held ~1-4%, min BT wake margin ~1.0ms of the 1.8ms lead.
* Baseline: PM + auto light sleep with CPU power-down still on (as previously shipped): 22 disconnects in
  3 minutes, ~every 17s (15s supervision timeout + reconnect); the device never reset. Every connection event
  is effectively being missed once light sleep starts.





