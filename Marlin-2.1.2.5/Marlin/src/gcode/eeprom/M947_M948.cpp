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

#include "../../inc/MarlinConfigPre.h"

#if ENABLED(LAST_THERMAL_FAULT)

#include "../gcode.h"
#include "../../feature/last_thermal_fault.h"

/** M947: Report the persistent last thermal fault. */
void GcodeSuite::M947() {
  LastThermalFault::report();
}

/** M948: Clear the persistent last thermal fault. */
void GcodeSuite::M948() {
  if (LastThermalFault::clear())
    SERIAL_ECHO_MSG("Last thermal fault cleared");
  else
    SERIAL_ERROR_MSG("Unable to clear last thermal fault");
}

#endif // LAST_THERMAL_FAULT
