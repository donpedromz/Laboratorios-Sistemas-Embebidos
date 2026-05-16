#ifndef BULK_STORAGE_H
#define BULK_STORAGE_H

#include "types.h"
#include <Arduino.h>

String bulkSerialize(const SensorReading readings[], uint8_t count);

#endif
