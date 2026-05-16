#include "sensors.h"
#include "config.h"
#include <DHT.h>

static DHT dht(PIN_DHT11, DHT11);

void sensorsInit() {
    Serial.println("[SENSORS] INIT => Iniciando DHT11...");
    dht.begin();
    Serial.printf("[SENSORS] INIT => Configurando pines: IR=%d, POT=%d\n", PIN_IR, PIN_POT);
    pinMode(PIN_IR, INPUT);
    pinMode(PIN_POT, INPUT);
    Serial.println("[SENSORS] OK => Sensores inicializados");
}

SensorReading sensorsRead() {
    SensorReading reading;
    reading.timestamp = millis();

    float temp = dht.readTemperature();
    float hum = dht.readHumidity();
    if (isnan(temp) || isnan(hum)) {
        Serial.println("[SENSORS] ERROR => Lectura DHT fallida (NaN)");
        reading.temperatura = -999.0f;
        reading.humedad = -999.0f;
    } else {
        reading.temperatura = temp;
        reading.humedad = hum;
    }

    reading.ir_proximidad = (float)analogRead(PIN_IR);
    reading.potenciometro = (float)analogRead(PIN_POT);

    Serial.printf("[SENSORS] READ => t=%.1fC h=%.1f%% ir=%.0f pot=%.0f ts=%lu\n",
                  reading.temperatura, reading.humedad,
                  reading.ir_proximidad, reading.potenciometro,
                  reading.timestamp);

    return reading;
}
