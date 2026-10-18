# Persistent Last Thermal Fault Record

## Purpose

This document records the implementation and validation of a persistent last-thermal-fault record for this Marlin 2.1.2.5 project.

The feature preserves diagnostic information across a reset or power cycle for these fatal thermal faults:

- `HEATING_FAILED`
- `THERMAL_RUNAWAY`
- `MINTEMP`
- `MAXTEMP`

The stored information is:

- Fault type
- Marlin heater ID
- Measured temperature immediately before shutdown
- Target temperature immediately before shutdown clears it

Only the first qualifying fatal thermal fault during a boot is recorded. A thermal fault normally halts the printer, so this avoids repeated EEPROM writes and preserves the original cause.

The current revision also changes the fatal-error sequence for those four recorded fault types:

1. Disable all heaters and capture/store the fault.
2. Sound the existing short alert pattern, but do not leave the beeper continuously on.
3. Keep the printer shut down until 15 seconds have elapsed from fault handling.
4. Reset the Mega 2560 automatically.
5. Finish the normal Marlin boot screens.
6. Show a blocking Full Graphics LCD error screen until the encoder's OK button is pressed.

The SD card is not involved. The record is held in the ATmega2560's internal EEPROM, so it is available with no SD card inserted.

## Target configuration inspected

The project is configured for:

- PlatformIO environment: `mega2560`
- MCU: ATmega2560, 16 MHz
- Board definition: `BOARD_RAMPS_14_EFB`
- Printer electronics: RAMPS 1.6 / Arduino Mega 2560 using RAMPS-compatible EFB wiring
- Extruders: 1
- Hotend thermistor: type 1
- Bed thermistor: type 1
- EEPROM settings: enabled
- Leveling: bilinear ABL
- Watchdog: enabled

Marlin uses its RAMPS 1.4 pin definition for the pin-compatible RAMPS 1.6 board.

## Existing protection settings

No thermal thresholds, timing values, PID values, printer geometry, motion settings, leveling data, or other printer settings were changed.

The relevant existing settings remain:

| Protection | Existing value |
|---|---:|
| Hotend MINTEMP | 5 C |
| Hotend MAXTEMP | 275 C |
| Bed MINTEMP | 5 C |
| Bed MAXTEMP | 150 C |
| Hotend thermal-runaway period | 40 seconds |
| Hotend thermal-runaway hysteresis | 4 C |
| Bed thermal-runaway period | 20 seconds |
| Bed thermal-runaway hysteresis | 2 C |
| Hotend heating-watch period | 40 seconds |
| Required hotend rise during watch | 2 C |
| Bed heating-watch period | 60 seconds |
| Required bed rise during watch | 2 C |

## Implementation approach

### Automatic reset timing and shutdown state

`LAST_THERMAL_FAULT_AUTO_REBOOT_MS` is set to `15000` in `Configuration_adv.h`.

The automatic reboot is applied only when `_temp_error()` is handling one of the four recordable fault types while Marlin is running. Unrelated calls to `kill()`, including `M112`, retain their original halt/reset behavior.

The deadline is established before the EEPROM operation. The existing kill path then:

- Keeps all heaters disabled.
- Emits the existing 20 short 25 ms alert chirps with 80 ms gaps.
- Explicitly turns the beeper off instead of enabling a continuous tone.
- Flushes the serial error message.
- Disables interrupts, reiterates heater and cutter shutdown, and disables the extruder steppers.
- Waits out the remainder of the 15-second deadline using interrupt-independent microsecond delays.
- Refreshes the hardware watchdog every 100 ms during that final hold.
- Performs a prompt hardware reset.

The delay is an absolute `millis()` deadline, not an additional 15-second sleep after beeping and EEPROM storage. Small hardware and instruction timing tolerances still apply.

### Full Graphics boot acknowledgment

After the normal Marlin boot splash completes, a valid CRC-checked record produces a blocking 128x64 Full Graphics screen with this layout:

```text
             ERROR
       THERMAL RUNAWAY
          HEATER: BED
      NOW:55.4 SET:60.0C
  +--------------------+
  |PRESS OK TO CONTINUE|
  +--------------------+
```

The reason changes to `HEATING FAILED`, `MINTEMP`, or `MAXTEMP` as appropriate. Heater and captured measured/target values come from the validated EEPROM record.

Marlin remains in `MF_INITIALIZING` while the screen is present. The boot-only wait services the encoder/LCD and refreshes the watchdog, but the normal G-code loop is not running. Heater targets are zero, and queued host commands cannot execute behind the warning.

Pressing OK acknowledges the warning for that boot only. It does **not** erase the forensic record. Use `M948` after diagnosing and repairing the cause when the record should be cleared. Consequently, a valid record will be shown again after every reboot until `M948` succeeds.

If a physical MINTEMP/MAXTEMP condition still exists when OK is pressed, normal temperature monitoring resumes after setup and Marlin may immediately fault, save, and reboot again. Repair the sensor/wiring before acknowledging a persistent sensor fault.

### Fault capture location

Marlin routes the relevant errors through `Temperature::_temp_error()`.

The implementation adds a `ThermalFaultType` argument to identify the reason explicitly rather than trying to infer it from a translated display string or flash-string pointer.

The following paths are tagged for storage:

| Marlin path | Stored type |
|---|---|
| Hotend, bed, and chamber heating watch | `HEATING_FAILED` |
| PID autotune heating watch | `HEATING_FAILED` |
| Thermal-runaway state machine | `THERMAL_RUNAWAY` |
| PID autotune temperature collapse | `THERMAL_RUNAWAY` |
| `mintemp_error()` | `MINTEMP` |
| `maxtemp_error()` | `MAXTEMP` |

Other temperature-related shutdowns, such as redundant-sensor mismatch, variance malfunction, or cooling failure, retain their existing behavior but are not written as one of the requested four fault types.

### Capturing temperatures before shutdown

For ordinary heater faults, the measured and target temperatures are copied into local variables before `disable_all_heaters()` is called.

PID autotune does not necessarily put its requested tuning temperature into the normal heater target field. Its current and requested target values are therefore passed directly to `_temp_error()` from the autotune loop.

The order is:

1. Identify the fault.
2. Capture heater ID, measured temperature, and target temperature into local variables.
3. Disable every heater.
4. Store the already-captured values.
5. Continue the existing Marlin kill path.

This ensures the recorded target is not zeroed by shutdown before it can be captured.

### Heater-off invariant

`Temperature::disable_all_heaters()` is called before any fault-record EEPROM write. It:

- Sets hotend targets to zero
- Sets bed and chamber targets to zero where present
- Clears software PWM values
- Drives heater output pins low

`LastThermalFault::store()` calls `disable_all_heaters()` again defensively before accessing EEPROM.

`M948`, which clears the record and therefore also writes EEPROM, applies the same rule and disables all heaters before clearing the commit marker. This means `M948` must be treated as a heater-off command.

`M947` only reads EEPROM and does not alter heater state.

## EEPROM allocation

The ATmega2560 has 4096 bytes of EEPROM, addressed from `0x0000` through `0x0FFF`.

The last-fault record reserves 16 bytes:

| Region | Address |
|---|---|
| Last thermal fault record | `0x0FF0` through `0x0FFF` |

Marlin's regular settings begin at EEPROM offset 100. The last-fault data is independent of `SettingsData`, so:

- `M500` does not overwrite the fault record.
- `M501` does not load it into printer settings.
- `M502` does not clear it.
- The Marlin settings EEPROM version did not need to be incremented.

Collision protection includes:

- A compile-time assertion on AVR that regular settings finish before the reserved record.
- A runtime comparison between the record address and `MarlinSettings::storage_end()` before reading or writing.
- Continued use of the existing Marlin collision checks for print statistics and SD firmware-update flags.
- Accommodation for UBL if it is enabled later.

UBL normally reserves 129 bytes at the top of EEPROM for its Mesh Allocation Table area. With this feature enabled, the last-fault record is placed immediately below that UBL reservation, and `meshes_end` is moved down accordingly. The current bilinear-leveling configuration stores its data in normal `SettingsData` and does not use the UBL area.

## Record format

The packed record is exactly 16 bytes:

| Field | Size | Description |
|---|---:|---|
| Commit magic | 2 bytes | `0x4C46`; written last |
| Version | 1 byte | Current format version is 1 |
| Fault type | 1 byte | Heating failed, runaway, MINTEMP, or MAXTEMP |
| Heater ID | 1 byte | Marlin heater ID, including E0 or BED |
| Flags | 1 byte | Must be zero |
| Measured temperature | 2 bytes | Signed temperature in tenths of a degree Celsius |
| Target temperature | 2 bytes | Signed temperature in tenths of a degree Celsius |
| Reserved | 4 bytes | Must be zero |
| CRC-16 | 2 bytes | CRC-16/CCITT over version through reserved bytes |

Temperatures are stored as signed fixed-point tenths of a degree rather than floating-point values. This keeps the record small and stable across compiler or platform floating-point representations.

## Interrupted-write protection

The storage operation is transaction-like:

1. Write an invalid zero commit marker over any prior marker.
2. Write and verify the versioned payload and CRC.
3. Write the valid magic marker last.

The AVR EEPROM backend verifies every changed byte after writing it. The record reader accepts a record only when all of these checks pass:

- Commit magic is exactly correct.
- Format version is supported.
- Fault type is one of the four supported values.
- Heater ID is in Marlin's supported heater-ID range.
- Flags and reserved bytes are zero.
- Stored CRC matches the calculated payload CRC.

Consequences of interruption:

- Interruption before invalidation completes leaves either the previous complete record or an invalid marker.
- Interruption while writing the payload leaves an invalid marker.
- Interruption during the two-byte final commit leaves either the completed marker or a non-matching partial marker.
- A partial or corrupted new record is never treated as valid.

With a single-slot record, an interruption while replacing an old record may invalidate the old record. The design prioritizes never accepting partial new data as valid. A future enhancement could use two alternating slots if retaining the previous valid record through an interrupted replacement becomes a requirement.

## Watchdog and interrupt handling

The raw ADC samples are accumulated in interrupt context, but Marlin calls `updateTemperaturesFromRawValues()` and raises MINTEMP/MAXTEMP and the other managed thermal errors from normal program context.

The feature therefore does not write EEPROM from an ISR.

Interrupts remain enabled during storage. This is important for timer and serial operation and avoids extending an interrupt-disabled critical section around relatively slow EEPROM writes.

The configured AVR watchdog timeout is four seconds. A complete 16-byte AVR EEPROM update is only on the order of tens of milliseconds. Nevertheless, the implementation explicitly refreshes the watchdog:

- After heaters are disabled
- After invalidating the marker
- After writing the payload
- After writing the final commit marker
- After clearing a record

The existing AVR EEPROM backend also avoids unnecessary writes to bytes that already contain the requested value.

During the automatic-reset hold, timer interrupts have already been stopped by Marlin's final kill path, so the code does not rely on `millis()` or Arduino `delay()`. It uses `DELAY_US(1000)` slices and refreshes the watchdog every 100 ms.

The ordinary AVR `hal.reboot()` waits for the configured watchdog period, which is four seconds in this build. Adding that wait after the 15-second deadline would reset at about 19 seconds. A dedicated AVR `reboot_now()` therefore switches the watchdog to its shortest 15 ms hardware-reset period. This prompt-reset helper is used only by the new timed thermal-fault path; other reboot and kill behavior is unchanged.

The post-boot acknowledgment loop runs with interrupts enabled and refreshes the watchdog directly because Marlin intentionally omits normal UI work from `idle()` while `marlin_state` is `MF_INITIALIZING`.

## G-code commands

### `M947` - report

Reports the persistent last thermal fault.

Examples:

```text
M947
Last thermal fault: none
```

```text
M947
Last thermal fault: type=HEATING_FAILED heater=E0 measured=24.6C target=50.0C
```

If the commit marker exists but the format or CRC is invalid:

```text
M947
Last thermal fault: invalid record (ignored)
```

### `M948` - clear

Disables all heaters and invalidates the persistent record:

```text
M948
echo:Last thermal fault cleared
```

The clear operation changes only the fault-record marker. It does not reset Marlin settings.

### Command-number conflict audit

Before these commands were added, searches of the original Git revision found no occurrence of:

- `M947`
- `M948`
- `case 947:`
- `case 948:`

The new commands were added to the command documentation, declarations, dispatcher, and their own implementation source.

## Files changed

### New files

- `Marlin/src/feature/last_thermal_fault.h`
  - Fault-type enumeration
  - Public feature interface
  - EEPROM reservation constants
- `Marlin/src/feature/last_thermal_fault.cpp`
  - Record layout
  - CRC and validation
  - Transactional store/clear logic
  - Serial report formatting
  - Validated-record Full Graphics boot screen and acknowledgment loop
- `Marlin/src/gcode/eeprom/M947_M948.cpp`
  - `M947` report command
  - `M948` clear command

### Modified files

- `Marlin/Configuration_adv.h`
  - Enables `LAST_THERMAL_FAULT`
  - Sets the automatic thermal-fault reboot interval to 15 seconds
- `ini/features.ini`
  - Adds the feature source files to PlatformIO's conditional source filter
- `Marlin/src/module/temperature.h`
  - Adds fault-type-aware `_temp_error()` overloads
- `Marlin/src/module/temperature.cpp`
  - Tags requested fault types
  - Captures measured and target temperatures before shutdown
  - Stores the record after heaters are disabled
  - Stops the beeper after the finite alert pattern
  - Starts the thermal-fault-only automatic-reset deadline
- `Marlin/src/MarlinCore.h` and `Marlin/src/MarlinCore.cpp`
  - Carry the optional absolute reboot deadline through `kill()` / `minkill()`
  - Hold outputs off and refresh the watchdog until the deadline
  - Invoke the validated-record LCD screen after the normal boot splash
- `Marlin/src/HAL/AVR/HAL.h` and `Marlin/src/HAL/AVR/HAL.cpp`
  - Add the shortest-period watchdog reset used by the timed fault path
- `Marlin/src/module/settings.h`
  - Exposes the end of the normal settings allocation
- `Marlin/src/module/settings.cpp`
  - Implements the settings-end query
  - Adds the AVR collision assertion
  - Accounts for the record in UBL's allocation boundary
- `Marlin/src/inc/SanityCheck.h`
  - Requires `EEPROM_SETTINGS` when the feature is enabled
- `Marlin/src/gcode/gcode.h`
  - Documents and declares M947/M948
- `Marlin/src/gcode/gcode.cpp`
  - Dispatches M947/M948

## Build validation

The project was built with:

```powershell
platformio run -e mega2560
```

Result:

```text
Environment    Status
mega2560       SUCCESS
```

Final memory use:

| Resource | Used | Available | Percentage |
|---|---:|---:|---:|
| SRAM | 4,954 bytes | 8,192 bytes | 60.5% |
| Flash | 146,994 bytes | 253,952 bytes | 57.9% |

For comparison, the clean build before the feature used:

- SRAM: 4,869 bytes
- Flash: 143,334 bytes

Total feature cost in this configuration relative to the clean build:

- SRAM: 85 bytes
- Flash: 3,660 bytes

The automatic-reset and Full Graphics acknowledgment enhancement added 82 bytes of SRAM and 1,358 bytes of flash beyond the previously committed M947/M948 record implementation.

Generated firmware:

```text
.pio/build/mega2560/firmware.hex
```

SHA-256:

```text
A544886330F89B015C1761E5B61B019030336EED9C508A186288C97FFC52290C
```

The hash applies to the build produced during this implementation and will change after any source or configuration change.

## Normal operating procedure

After a thermal shutdown:

1. Do not bypass the fault or immediately restart heating.
2. Confirm the heater indicators and outputs turn off; the short chirps should stop after about two seconds.
3. Let the controller reset automatically at about 15 seconds.
4. Read the reason, heater, measured value, and target value on the post-splash ERROR screen.
5. Correct the underlying fault, especially before acknowledging a persistent sensor problem.
6. Press the encoder OK button to finish startup.
7. Connect over serial and run `M947`; save its full report in the maintenance log.
8. Run `M948` while the printer is idle to clear the record.
9. Run `M947` again and confirm it reports `none`.

`M500`, `M501`, and `M502` are not needed for this feature.

## Safe test procedure

### General precautions

- Supervise every thermal test continuously.
- Keep a suitable fire extinguisher nearby.
- Never defeat Marlin's thermal protection limits.
- Never increase the protection periods or lower the required temperature changes for testing.
- Never connect or disconnect a heater, thermistor, or RAMPS header while heater power is present.
- Use a prewired switch or jumper installed only while all power is off if a thermistor circuit must be opened or shorted.
- For sensor-fault tests, physically remove or switch off the 12/24 V heater supply and keep only the Mega logic powered over USB.
- Verify heater MOSFET LEDs and heater outputs turn off immediately when a fault occurs.
- Verify the beeper stops after the finite alert pattern and the board resets at approximately 15 seconds.
- Before pressing OK after a sensor fault, restore the thermistor to a valid, safe state so the printer does not immediately fault again.
- Perform one test at a time, allow the automatic reset, inspect with `M947`, and clear with `M948` before the next test.

### Low-energy automatic-reset and LCD test

This is the preferred test for the new behavior because the heater power supply remains physically off.

1. Turn off and isolate the 12/24 V heater supply. Keep only the Mega 2560 logic powered over USB.
2. With all power off beforehand, install a safe series switch in the hotend thermistor circuit; begin with it closed so the sensor reads normally.
3. Restore USB logic power, press OK if an older record is shown, then run `M948` and verify `M947` reports `none`.
4. Send `M104 S50` and open the prewired thermistor switch.
5. Confirm `MINTEMP`, immediate heater shutdown, a finite alert lasting about two seconds, and automatic reset at approximately 15 seconds.
6. Close the thermistor switch again **before** pressing OK.
7. After the ordinary boot splash, verify the LCD shows `ERROR`, `MINTEMP`, `HEATER: E0`, captured `NOW` / `SET` values, and the boxed `PRESS OK TO CONTINUE` prompt.
8. Press the encoder button and run `M947`; verify the serial record matches the LCD.
9. Run `M948`, followed by `M947`, and verify the final response is `Last thermal fault: none`.

Never open the thermistor circuit while heater power is available. This test deliberately removes heater energy so a wiring mistake cannot command a live heater during validation.

### Initial command test

After flashing and rebooting:

```text
M947
M948
M947
```

The final response should be `Last thermal fault: none`.

Remember that `M948` intentionally switches every heater off.

### Heating-failed test

This test does not require a hot heater.

1. Keep the hotend thermistor normally connected.
2. Physically disconnect or switch off the main heater supply while keeping USB logic power connected.
3. Run `M948`.
4. Send `M104 S50`.
5. The measured temperature cannot rise by the configured 2 C within 40 seconds, so Marlin should issue `Heating failed`, shut down, and reset about 15 seconds later.
6. Verify the boot ERROR screen, press OK, and run `M947`.

Expected type and target:

```text
type=HEATING_FAILED heater=E0 ... target=50.0C
```

### MINTEMP test

1. Turn off and physically isolate heater power.
2. With all power off, arrange a safe switch in series with the hotend thermistor circuit.
3. Restore USB logic power with the thermistor connected.
4. Run `M948` and then `M104 S50`.
5. Open the prewired thermistor switch.
6. Marlin should issue `MINTEMP`, shut down, and reset about 15 seconds later.
7. Restore the thermistor circuit, verify the boot ERROR screen, press OK, and run `M947`.

For the configured NTC thermistor input, an open circuit represents a very low temperature. Do not unplug the header by hand while heater power is present.

### MAXTEMP test

1. Turn off and physically isolate heater power.
2. With all power off, arrange a safe switch that shorts only the two pins of the hotend thermistor input.
3. Verify the header and wiring before applying USB logic power.
4. Start with the switch open.
5. Run `M948` and optionally `M104 S50` so a nonzero target can be verified.
6. Close the thermistor-input switch.
7. Marlin should issue `MAXTEMP`, shut down, and reset about 15 seconds later.
8. Remove the short, verify the boot ERROR screen, press OK, and run `M947`.

For the configured NTC thermistor input, a short represents a very high temperature. Never short an arbitrary RAMPS pin to ground; use only the verified two-pin thermistor header.

### Thermal-runaway test

The heated bed provides a lower-temperature practical test than the hotend.

1. Keep the bed thermistor and heater normally connected.
2. Keep the Mega connected by USB so the controller remains powered when the heater PSU is switched off.
3. Run `M948`.
4. Send `M140 S40`.
5. Wait for the bed to reach and stabilize at 40 C.
6. Use the PSU's normal switch to turn off the main heater supply without touching wiring.
7. Leave USB logic power connected and do not clear the target.
8. After the bed falls more than the configured 2 C hysteresis for the configured 20-second period, Marlin should issue `THERMAL RUNAWAY`, shut down, and reset about 15 seconds later.
9. Verify the boot ERROR screen, press OK, and run `M947`.

Expected type:

```text
type=THERMAL_RUNAWAY heater=BED
```

If switching off the main PSU also removes controller logic power on the particular wiring arrangement, do not use this procedure until an electrically competent person has arranged independent, safe logic power.

## Maintenance notes

- The record format is versioned. If fields are added later, increment `FAULT_VERSION` and update validation/reporting.
- Keep the commit marker outside the CRC payload and write it last.
- Keep `StoredThermalFault` exactly equal to `LastThermalFault::eeprom_size`; the static assertion enforces this.
- If another feature claims top-of-EEPROM storage, update the allocation logic and add another collision check before enabling it.
- Do not move the storage call ahead of `disable_all_heaters()`.
- Do not call `LastThermalFault::store()` from an ISR.
- Preserve watchdog refreshes if the record becomes larger.
- If command numbers M947/M948 are assigned by a future Marlin upgrade, select new unused numbers and update the dispatcher, declarations, implementation filenames, documentation, and this handoff document together.

## Repository state note

At the time of this work, the Git repository already contained unrelated items outside this Marlin project (`../ThingsToPrint` and `../.gitignore`). They were not modified as part of this implementation.
