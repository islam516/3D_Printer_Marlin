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
#pragma once

#include "../inc/MarlinConfig.h"

enum ThermalFaultType : uint8_t {
  THERMAL_FAULT_NONE,
  THERMAL_FAULT_HEATING_FAILED,
  THERMAL_FAULT_RUNAWAY,
  THERMAL_FAULT_MINTEMP,
  THERMAL_FAULT_MAXTEMP
};

#if ENABLED(LAST_THERMAL_FAULT)

  class LastThermalFault {
    public:
      // Keep the record outside Marlin settings and, for UBL, below its
      // 129-byte Mesh Allocation Table reservation at the top of EEPROM.
      static constexpr uint8_t eeprom_size = 16;
      static constexpr uint16_t eeprom_tail_reserve = TERN(AUTO_BED_LEVELING_UBL, 129, 0);

      static int storage_address();
      static bool store(const ThermalFaultType type, const int8_t heater_id, const float measured, const float target);
      static bool clear();
      static void report();
  };

#endif
