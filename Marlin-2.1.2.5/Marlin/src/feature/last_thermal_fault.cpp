/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2020 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * Based on Sprinter and grbl.
 * Copyright (c) 2011 Camiel Gubbels / Erik van der Zalm
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "../inc/MarlinConfigPre.h"

#if ENABLED(LAST_THERMAL_FAULT)

#include "last_thermal_fault.h"

#include "../HAL/shared/eeprom_api.h"
#include "../core/serial.h"
#include "../libs/crc16.h"
#include "../module/settings.h"
#include "../module/temperature.h"

#if ALL(HAS_MARLINUI_U8GLIB, HAS_MARLINUI_MENU)
  #include "../lcd/marlinui.h"
  #include "../lcd/dogm/marlinui_DOGM.h"
  #include "../lcd/lcdprint.h"
#endif

#include <stddef.h>
#include <string.h>

namespace {

  constexpr uint16_t FAULT_MAGIC = 0x4C46; // "FL"; written only after the payload is complete
  constexpr uint8_t FAULT_VERSION = 1;

  struct __attribute__((packed)) StoredThermalFault {
    uint16_t magic;
    uint8_t version;
    uint8_t type;
    int8_t heater_id;
    uint8_t flags;
    int16_t measured_x10;
    int16_t target_x10;
    uint8_t reserved[4];
    uint16_t crc;
  };

  static_assert(sizeof(StoredThermalFault) == LastThermalFault::eeprom_size, "Unexpected last-fault record size.");

  enum RecordStatus : uint8_t { RECORD_EMPTY, RECORD_VALID, RECORD_INVALID };

  bool stored_this_boot = false;

  int16_t to_deci_celsius(const float value) {
    const float scaled = value * 10.0f;
    if (scaled >= 32767.0f) return 32767;
    if (scaled <= -32768.0f) return -32768;
    return int16_t(scaled + (scaled < 0 ? -0.5f : 0.5f));
  }

  uint16_t payload_crc(const StoredThermalFault &record) {
    uint16_t crc = 0;
    crc16(&crc, &record.version, offsetof(StoredThermalFault, crc) - offsetof(StoredThermalFault, version));
    return crc;
  }

  bool storage_available() {
    const int address = LastThermalFault::storage_address();
    return address >= int(settings.storage_end()) && address >= 0;
  }

  RecordStatus read_record(StoredThermalFault &record) {
    if (!storage_available() || !persistentStore.access_start()) return RECORD_INVALID;

    int pos = LastThermalFault::storage_address();
    uint16_t ignored_crc = 0;
    const bool error = persistentStore.read_data(pos, (uint8_t*)&record, sizeof(record), &ignored_crc);
    persistentStore.access_finish();
    if (error) return RECORD_INVALID;

    if (record.magic == 0 || record.magic == 0xFFFF) return RECORD_EMPTY;
    if (record.magic != FAULT_MAGIC) return RECORD_INVALID;
    if (record.version != FAULT_VERSION || !WITHIN(record.type, THERMAL_FAULT_HEATING_FAILED, THERMAL_FAULT_MAXTEMP)) return RECORD_INVALID;
    if (!WITHIN(record.heater_id, HID_REDUNDANT, HID_E7) || record.flags) return RECORD_INVALID;
    for (uint8_t i = 0; i < COUNT(record.reserved); ++i)
      if (record.reserved[i]) return RECORD_INVALID;
    return payload_crc(record) == record.crc ? RECORD_VALID : RECORD_INVALID;
  }

  void print_fault_type(const uint8_t type) {
    switch (type) {
      case THERMAL_FAULT_HEATING_FAILED: SERIAL_ECHOPGM("HEATING_FAILED");   break;
      case THERMAL_FAULT_RUNAWAY:        SERIAL_ECHOPGM("THERMAL_RUNAWAY"); break;
      case THERMAL_FAULT_MINTEMP:        SERIAL_ECHOPGM("MINTEMP");         break;
      case THERMAL_FAULT_MAXTEMP:        SERIAL_ECHOPGM("MAXTEMP");         break;
    }
  }

  void print_heater_id(const int8_t heater_id) {
    switch (heater_id) {
      case HID_REDUNDANT: SERIAL_ECHOPGM("REDUNDANT"); break;
      case HID_BOARD:     SERIAL_ECHOPGM("BOARD");     break;
      case HID_COOLER:    SERIAL_ECHOPGM("COOLER");    break;
      case HID_PROBE:     SERIAL_ECHOPGM("PROBE");     break;
      case HID_CHAMBER:   SERIAL_ECHOPGM("CHAMBER");   break;
      case HID_BED:       SERIAL_ECHOPGM("BED");       break;
      default:            SERIAL_CHAR('E'); SERIAL_ECHO(int(heater_id)); break;
    }
  }

  #if ALL(HAS_MARLINUI_U8GLIB, HAS_MARLINUI_MENU)

    StoredThermalFault boot_fault;
    bool boot_fault_acknowledged;
    char boot_heater_line[21], boot_temperature_line[24];

    FSTR_P boot_fault_reason(const uint8_t type) {
      switch (type) {
        case THERMAL_FAULT_HEATING_FAILED: return F("HEATING FAILED");
        case THERMAL_FAULT_RUNAWAY:        return F("THERMAL RUNAWAY");
        case THERMAL_FAULT_MINTEMP:        return F("MINTEMP");
        case THERMAL_FAULT_MAXTEMP:        return F("MAXTEMP");
        default:                           return F("UNKNOWN");
      }
    }

    FSTR_P boot_heater_name(const int8_t heater_id) {
      switch (heater_id) {
        case HID_REDUNDANT: return F("REDUNDANT");
        case HID_BOARD:     return F("BOARD");
        case HID_COOLER:    return F("COOLER");
        case HID_PROBE:     return F("PROBE");
        case HID_CHAMBER:   return F("CHAMBER");
        case HID_BED:       return F("BED");
        default:            return nullptr;
      }
    }

    void format_deci_celsius(char * const out, const size_t size, const int16_t value) {
      const bool negative = value < 0;
      const uint16_t magnitude = negative ? uint16_t(-int32_t(value)) : uint16_t(value);
      snprintf_P(out, size, negative ? PSTR("-%u.%u") : PSTR("%u.%u"), magnitude / 10, magnitude % 10);
    }

    void draw_centered(FSTR_P const text, const u8g_uint_t y) {
      const int width = calculateWidth(text);
      lcd_put_u8str(width < LCD_PIXEL_WIDTH ? (LCD_PIXEL_WIDTH - width) / 2 : 0, y, text);
    }

    void draw_centered(const char * const text, const u8g_uint_t y) {
      const uint16_t width = strlen(text) * MENU_FONT_WIDTH;
      const u8g_uint_t x = width < LCD_PIXEL_WIDTH ? (LCD_PIXEL_WIDTH - width) / 2 : 0;
      lcd_moveto(x, y);
      lcd_put_u8str_max(text, LCD_PIXEL_WIDTH - x);
    }

    void draw_boot_fault_screen() {
      if (ui.use_click()) boot_fault_acknowledged = true;
      if (boot_fault_acknowledged) return;

      ui.defer_status_screen();
      ui.set_font(FONT_MENU);

      if (PAGE_CONTAINS(0, 11)) {
        u8g.setColorIndex(1);
        u8g.drawBox(0, 0, LCD_PIXEL_WIDTH, 12);
        u8g.setColorIndex(0);
        draw_centered(F("ERROR"), 10);
        u8g.setColorIndex(1);
      }

      if (PAGE_CONTAINS(12, 24)) draw_centered(boot_fault_reason(boot_fault.type), 22);
      if (PAGE_CONTAINS(25, 36)) draw_centered(boot_heater_line, 34);
      if (PAGE_CONTAINS(37, 48)) draw_centered(boot_temperature_line, 46);

      if (PAGE_CONTAINS(49, 63)) {
        u8g.drawFrame(1, 49, LCD_PIXEL_WIDTH - 2, 15);
        draw_centered(F("PRESS OK TO CONTINUE"), 61);
      }
    }

  #endif

} // namespace

int LastThermalFault::storage_address() {
  return int(persistentStore.capacity()) - eeprom_tail_reserve - eeprom_size;
}

bool LastThermalFault::store(const ThermalFaultType type, const int8_t heater_id, const float measured, const float target) {
  // This method is deliberately safe to call only from normal context. Thermal
  // faults are raised after the ADC ISR publishes a sample, never from the ISR.
  // Keep interrupts enabled while EEPROM is busy, but force every heater off first.
  thermalManager.disable_all_heaters();
  hal.watchdog_refresh();

  if (stored_this_boot || type == THERMAL_FAULT_NONE || !storage_available()) return false;

  StoredThermalFault record = {};
  record.version = FAULT_VERSION;
  record.type = type;
  record.heater_id = heater_id;
  record.measured_x10 = to_deci_celsius(measured);
  record.target_x10 = to_deci_celsius(target);
  record.crc = payload_crc(record);

  if (!persistentStore.access_start()) return false;

  const int address = storage_address();
  const uint16_t invalid_magic = 0;

  // Transaction order: invalidate old marker, write and verify payload, then
  // commit the magic marker. A reset at any earlier point leaves no valid record.
  bool error = persistentStore.write_data(address, (const uint8_t*)&invalid_magic, sizeof(invalid_magic));
  hal.watchdog_refresh();

  if (!error) {
    int pos = address + offsetof(StoredThermalFault, version);
    uint16_t ignored_crc = 0;
    error = persistentStore.write_data(pos, &record.version, sizeof(record) - offsetof(StoredThermalFault, version), &ignored_crc);
  }
  hal.watchdog_refresh();

  if (!error)
    error = persistentStore.write_data(address, (const uint8_t*)&FAULT_MAGIC, sizeof(FAULT_MAGIC));

  persistentStore.access_finish();
  hal.watchdog_refresh();

  stored_this_boot = !error;
  return !error;
}

bool LastThermalFault::clear() {
  // Clearing also writes EEPROM, so apply the same heater-off invariant.
  thermalManager.disable_all_heaters();
  hal.watchdog_refresh();

  if (!storage_available() || !persistentStore.access_start()) return false;
  const uint16_t invalid_magic = 0;
  const bool error = persistentStore.write_data(storage_address(), (const uint8_t*)&invalid_magic, sizeof(invalid_magic));
  persistentStore.access_finish();
  hal.watchdog_refresh();
  if (!error) stored_this_boot = false;
  return !error;
}

void LastThermalFault::report() {
  StoredThermalFault record;
  const RecordStatus status = read_record(record);

  SERIAL_ECHOPGM("Last thermal fault: ");
  if (status == RECORD_EMPTY) {
    SERIAL_ECHOLNPGM("none");
    return;
  }
  if (status == RECORD_INVALID) {
    SERIAL_ECHOLNPGM("invalid record (ignored)");
    return;
  }

  SERIAL_ECHOPGM("type=");
  print_fault_type(record.type);
  SERIAL_ECHOPGM(" heater=");
  print_heater_id(record.heater_id);
  SERIAL_ECHOPGM(" measured=");
  SERIAL_ECHO_F(record.measured_x10 * 0.1f, 1);
  SERIAL_ECHOPGM("C target=");
  SERIAL_ECHO_F(record.target_x10 * 0.1f, 1);
  SERIAL_ECHOLNPGM("C");
}

#if ALL(HAS_MARLINUI_U8GLIB, HAS_MARLINUI_MENU)

  void LastThermalFault::show_boot_screen() {
    StoredThermalFault record;
    if (read_record(record) != RECORD_VALID) return;

    boot_fault = record;

    strcpy_P(boot_heater_line, PSTR("HEATER: "));
    const FSTR_P heater_name = boot_heater_name(record.heater_id);
    if (heater_name)
      strcat_P(boot_heater_line, FTOP(heater_name));
    else
      snprintf_P(boot_heater_line, sizeof(boot_heater_line), PSTR("HEATER: E%i"), int(record.heater_id));

    char measured[8], target[8];
    format_deci_celsius(measured, sizeof(measured), record.measured_x10);
    format_deci_celsius(target, sizeof(target), record.target_x10);
    snprintf_P(boot_temperature_line, sizeof(boot_temperature_line), PSTR("NOW:%s SET:%sC"), measured, target);

    // Ignore a button held through startup, then block setup until a fresh
    // encoder click. Marlin remains MF_INITIALIZING, so queued G-code cannot
    // run behind this screen and Temperature::task only services the watchdog.
    while (ui.button_pressed()) {
      hal.watchdog_refresh();
      delay(10);
    }
    (void)ui.use_click();

    boot_fault_acknowledged = false;
    ui.goto_screen(draw_boot_fault_screen);
    ui.defer_status_screen();
    while (!boot_fault_acknowledged) {
      ui.update();
      hal.watchdog_refresh();
      delay(5);
    }

    while (ui.button_pressed()) {
      hal.watchdog_refresh();
      delay(10);
    }
    ui.return_to_status();
  }

#endif

#endif // LAST_THERMAL_FAULT
