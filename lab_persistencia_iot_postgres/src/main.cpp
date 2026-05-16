#include <Arduino.h>
#include "config.h"
#include "types.h"
#include "globals.h"
#include "sensors.h"
#include "network.h"
#include "bulk_storage.h"

QueueHandle_t sensorQueue;
QueueHandle_t systemMsgQueue;
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

static void sensorTask(void* parameter) {
    SensorReading localBuffer[BULK_SIZE];
    uint8_t idx = 0;
    Serial.println("[SENSOR_TASK] INIT => Tarea de sensores iniciada");

    while (true) {
        localBuffer[idx] = sensorsRead();
        idx++;

        if (idx >= BULK_SIZE) {
            Serial.printf("[SENSOR_TASK] INFO => Bulk completo (%d lecturas). Enviando a cola...\n", BULK_SIZE);
            if (xQueueSend(sensorQueue, &localBuffer, portMAX_DELAY) == pdTRUE) {
                Serial.println("[SENSOR_TASK] OK => Bulk enviado a cola");
            } else {
                Serial.println("[SENSOR_TASK] ERROR => Fallo al enviar bulk a cola");
            }
            idx = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(READ_INTERVAL_MS));
    }
}

static void serialTask(void* parameter) {
    SystemMessageBulk msgBulk;
    msgBulk.count = 0;
    unsigned long lastSendTime = millis();

    Serial.println("[SERIAL_TASK] INIT => Tarea de lectura serial iniciada. Escribe mensajes para enviar bulk.");

    while (true) {
        if (Serial.available()) {
            String line = Serial.readStringUntil('\n');
            line.trim();
            if (line.length() > 0 && line.length() < MSG_BUFFER_SIZE) {
                strncpy(msgBulk.messages[msgBulk.count].text, line.c_str(), MSG_BUFFER_SIZE - 1);
                msgBulk.messages[msgBulk.count].text[MSG_BUFFER_SIZE - 1] = '\0';
                msgBulk.messages[msgBulk.count].timestamp = millis();
                Serial.printf("[SERIAL_TASK] RECV => Mensaje %d/%d: %s\n", msgBulk.count + 1, BULK_MSG_SIZE, msgBulk.messages[msgBulk.count].text);
                msgBulk.count++;
            }
        }

        bool bufferFull = (msgBulk.count >= BULK_MSG_SIZE);
        bool timeout = (msgBulk.count > 0) && ((millis() - lastSendTime) >= MSG_BULK_TIMEOUT_MS);

        if (bufferFull || timeout) {
            Serial.printf("[SERIAL_TASK] BULK => Enviando %d mensajes a cola de protocolo\n", msgBulk.count);
            if (xQueueSend(systemMsgQueue, &msgBulk, portMAX_DELAY) == pdTRUE) {
                Serial.println("[SERIAL_TASK] OK => Bulk mensajes enviado a cola");
            } else {
                Serial.println("[SERIAL_TASK] ERROR => Fallo al enviar bulk mensajes a cola");
            }
            msgBulk.count = 0;
            lastSendTime = millis();
        }

        vTaskDelay(pdMS_TO_TICKS(MSG_READ_INTERVAL_MS));
    }
}

static void protocolTask(void* parameter) {
    Serial.println("[PROTO_TASK] INIT => Tarea de protocolo iniciada");

    Serial.println("[PROTO_TASK] INIT => Conectando WiFi...");
    wifiConnect();
    while (!mqttConnect()) {
        Serial.println("[PROTO_TASK] ERROR => MQTT no conectado. Reintentando en 5s...");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    Serial.println("[PROTO_TASK] OK => WiFi y MQTT conectados");

    SensorReading receivedBulk[BULK_SIZE];
    SystemMessageBulk receivedMsgBulk;

    while (true) {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[PROTO_TASK] WARN => WiFi desconectado. Reconectando...");
            wifiConnect();
        }
        if (!mqttClient.connected()) {
            Serial.println("[PROTO_TASK] WARN => MQTT desconectado. Reconectando...");
            while (!mqttConnect()) {
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
            Serial.println("[PROTO_TASK] OK => MQTT reconectado");
        }

        mqttLoop();

        if (xQueueReceive(sensorQueue, &receivedBulk, pdMS_TO_TICKS(100)) == pdTRUE) {
            Serial.println("[PROTO_TASK] RECV => Bulk de sensores recibido de cola. Serializando...");
            String jsonPayload = bulkSerialize(receivedBulk, BULK_SIZE);
            Serial.printf("[PROTO_TASK] PUBLISH => Enviando %d bytes a %s\n", jsonPayload.length(), TOPIC_BULK_PUBLISH);
            if (mqttClient.publish(TOPIC_BULK_PUBLISH, jsonPayload.c_str())) {
                Serial.println("[PROTO_TASK] OK => Publicacion de sensores exitosa");
            } else {
                Serial.println("[PROTO_TASK] ERROR => Fallo al publicar bulk de sensores");
            }
        }

        if (xQueueReceive(systemMsgQueue, &receivedMsgBulk, pdMS_TO_TICKS(100)) == pdTRUE) {
            Serial.printf("[PROTO_TASK] RECV => Bulk de mensajes sistema recibido (%d msgs). Serializando...\n", receivedMsgBulk.count);
            String jsonPayload = "[";
            for (uint8_t i = 0; i < receivedMsgBulk.count; i++) {
                if (i > 0) jsonPayload += ",";
                jsonPayload += "{\"device_id\":\"" + String(DEVICE_ID) + "\",";
                jsonPayload += "\"msg\":\"" + String(receivedMsgBulk.messages[i].text) + "\",";
                jsonPayload += "\"timestamp\":" + String(receivedMsgBulk.messages[i].timestamp) + "}";
            }
            jsonPayload += "]";

            Serial.printf("[PROTO_TASK] PUBLISH => Enviando %d bytes a %s (%d mensajes)\n",
                          jsonPayload.length(), TOPIC_LOGS_PUBLISH, receivedMsgBulk.count);
            if (mqttClient.publish(TOPIC_LOGS_PUBLISH, jsonPayload.c_str())) {
                Serial.println("[PROTO_TASK] OK => Publicacion de mensajes sistema exitosa");
            } else {
                Serial.println("[PROTO_TASK] ERROR => Fallo al publicar bulk de mensajes sistema");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(1000);
    Serial.println("[MAIN] INIT => Iniciando sistema...");

    Serial.println("[MAIN] INIT => sensorsInit()");
    sensorsInit();

    Serial.println("[MAIN] INIT => networkInit()");
    networkInit();

    Serial.println("[MAIN] INIT => Creando sensorQueue...");
    sensorQueue = xQueueCreate(2, sizeof(SensorReading) * BULK_SIZE);
    if (sensorQueue == NULL) {
        Serial.println("[MAIN] ERROR => sensorQueue es NULL. Bucle infinito.");
        while (true) { delay(1000); }
    }
    Serial.println("[MAIN] OK => sensorQueue creada");

    Serial.println("[MAIN] INIT => Creando systemMsgQueue...");
    systemMsgQueue = xQueueCreate(2, sizeof(SystemMessageBulk));
    if (systemMsgQueue == NULL) {
        Serial.println("[MAIN] ERROR => systemMsgQueue es NULL. Bucle infinito.");
        while (true) { delay(1000); }
    }
    Serial.println("[MAIN] OK => systemMsgQueue creada");

    Serial.println("[MAIN] INIT => Lanzando tareas FreeRTOS...");
    xTaskCreatePinnedToCore(sensorTask, "sensorTask", 4096, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(serialTask, "serialTask", 4096, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(protocolTask, "protocolTask", 8192, NULL, 1, NULL, 0);
    Serial.println("[MAIN] OK => Tareas lanzadas. Setup completado.");
}

void loop() {
    vTaskDelay(portMAX_DELAY);
}
