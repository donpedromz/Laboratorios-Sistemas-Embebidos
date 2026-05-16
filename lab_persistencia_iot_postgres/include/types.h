#ifndef TYPES_H
#define TYPES_H

#include <Arduino.h>
#include "config.h"

struct SensorReading {
    float temperatura;
    float humedad;
    float ir_proximidad;
    float potenciometro;
    unsigned long timestamp;
};

struct SystemMessage {
    char text[MSG_BUFFER_SIZE];
    unsigned long timestamp;
};

struct SystemMessageBulk {
    SystemMessage messages[BULK_MSG_SIZE];
    uint8_t count;
};

#endif
