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





