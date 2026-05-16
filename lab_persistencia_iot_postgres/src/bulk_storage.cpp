#include "bulk_storage.h"
#include "config.h"
#include <ArduinoJson.h>

String bulkSerialize(const SensorReading readings[], uint8_t count) {
    Serial.printf("[BULK] INIT => Serializando %d lecturas...\n", count);
    JsonDocument doc;
    JsonArray array = doc.to<JsonArray>();

    for (uint8_t i = 0; i < count; i++) {
        JsonObject obj = array.add<JsonObject>();
        obj["device_id"] = DEVICE_ID;
        obj["idx"] = i;
        obj["temperatura"] = readings[i].temperatura;
        obj["humedad"] = readings[i].humedad;
        obj["ir_proximidad"] = readings[i].ir_proximidad;
        obj["potenciometro"] = readings[i].potenciometro;
        obj["timestamp_ms"] = readings[i].timestamp;
    }

    String output;
    serializeJson(doc, output);
    Serial.printf("[BULK] OK => Payload generado, %d bytes\n", output.length());
    return output;
}
