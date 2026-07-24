from pathlib import Path

Import("env")


def patch_sections_ld() -> None:
    platform = env.PioPlatform()
    package_dir = platform.get_package_dir("framework-arduinoespressif32-libs")
    if package_dir is None:
        print("FreeRTOS linker patch skipped: framework-arduinoespressif32-libs is not installed yet.")
        return

    sections_ld = Path(package_dir) / "esp32c3" / "ld" / "sections.ld"
    text = sections_ld.read_text()
    insert = (
        "    *libfreertos.a:tasks.*(.literal.prvGetExpectedIdleTime .text.prvGetExpectedIdleTime)\n"
        "    *libfreertos.a:tasks.*(.literal.vTaskStepTick .text.vTaskStepTick)\n"
    )
    if insert in text:
        return

    needle = "    *libfreertos.a:tasks.*(.literal.uxTaskGetSnapshotAll .text.uxTaskGetSnapshotAll)\n"
    if needle not in text:
        print(f"FreeRTOS linker patch skipped: insertion point not found in {sections_ld}.")
        return

    sections_ld.write_text(text.replace(needle, insert + needle))
    print(f"Patched {sections_ld} for FreeRTOS flash task sections.")


patch_sections_ld()
