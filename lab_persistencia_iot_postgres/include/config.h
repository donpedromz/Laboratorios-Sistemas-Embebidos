#ifndef CONFIG_H
#define CONFIG_H

#define WIFI_SSID "vKU028aYrHSoIRG"
#define WIFI_PASSWORD "]|mQk_>KZH8K@:~7nOV%"

#define MQTT_BROKER_HOST "192.168.0.7"
#define MQTT_BROKER_PORT 1883

#define MQTT_CLIENT_ID "esp32_iot_device"
#define MQTT_USER "iot_device"
#define MQTT_PASS "SecurePass123!"

#define TOPIC_BULK_PUBLISH    "sensores/bulk"
#define TOPIC_LOGS_PUBLISH    "logs/mensajes_sistema"

#define PIN_DHT11     5
#define PIN_IR        32
#define PIN_POT       33

#define BULK_SIZE           5
#define READ_INTERVAL_MS    1000
#define PUBLISH_INTERVAL_MS (BULK_SIZE * READ_INTERVAL_MS)

#define BULK_MSG_SIZE       3
#define MSG_BUFFER_SIZE     128
#define MSG_READ_INTERVAL_MS 100
#define MSG_BULK_TIMEOUT_MS 5000

#define DEVICE_ID "esp32_devkit_001"
#define SERIAL_BAUD 115200

#define ERR_WIFI_CONNECT    -1
#define ERR_MQTT_CONNECT    -2
#define ERR_SENSOR_READ     -3
#define OK_STATUS            0

#endif
