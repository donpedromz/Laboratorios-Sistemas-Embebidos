#include "network.h"
#include "config.h"
#include "globals.h"
#include <WiFi.h>

void networkInit() {
    Serial.println("[NET] INIT => Configurando servidor MQTT...");
    mqttClient.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
    mqttClient.setBufferSize(4096);
    Serial.printf("[NET] OK => Servidor MQTT %s:%d, buffer=4096\n", MQTT_BROKER_HOST, MQTT_BROKER_PORT);
}

bool wifiConnect() {
    Serial.printf("[NET] INIT => Conectando WiFi SSID=%s...\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 30) {
        vTaskDelay(pdMS_TO_TICKS(500));
        retries++;
    }
    bool ok = WiFi.status() == WL_CONNECTED;
    if (ok) {
        Serial.printf("[NET] OK => WiFi conectada, IP=%s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("[NET] ERROR => WiFi no conectada (timeout)");
    }
    return ok;
}

bool mqttConnect() {
    if (mqttClient.connected()) {
        return true;
    }
    Serial.printf("[NET] INIT => Conectando MQTT broker=%s:%d, client_id=%s...\n", MQTT_BROKER_HOST, MQTT_BROKER_PORT, MQTT_CLIENT_ID);
    if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)) {
        Serial.println("[NET] OK => MQTT conectado");
        return true;
    }
    Serial.println("[NET] ERROR => Fallo conexion MQTT");
    return false;
}

void mqttLoop() {
    mqttClient.loop();
}
