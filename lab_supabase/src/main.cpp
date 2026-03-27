#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// ===========================================================================
// CONFIGURACION
// ===========================================================================
const char* WIFI_SSID     = "";
const char* WIFI_PASSWORD = "";

// RabbitMQ via plugin MQTT (puerto 1883)
const char* MQTT_SERVER   = "";
const int   MQTT_PORT     = 1883;
const char* MQTT_USER     = "iot_device";
const char* MQTT_PASSWORD = "SecurePass123!";

// MQTT Topics para RabbitMQ
const char* TOPIC_READING = "sensor/ambient/reading";
const char* TOPIC_STATUS = "sensor/esp32/status";
const char* TOPIC_ERROR  = "sensor/esp32/error";

// DHT requiere GPIO con entrada/salida.
const int DHT_PIN = 4;
#define DHT_TYPE DHT11

// Intervalo de lectura del sensor (ms)
const int INTERVALO_LECTURA_MS = 3000;
const int INTERVALO_REINTENTO_MQTT_MS = 5000;

// ===========================================================================
// ESTRUCTURA DE DATOS - Lectura de temperatura y humedad
// ===========================================================================

struct EnvData {
  float  temperatureC;     // Temperatura en grados Celsius
  float  humidity;         // Humedad relativa en porcentaje
  int    readingNumber;    // Numero de lectura secuencial
  unsigned long espMillis; // Timestamp interno del ESP32
};

// ===========================================================================
// VARIABLES GLOBALES
// ===========================================================================

WiFiClient espClient;
PubSubClient mqttClient(espClient);
DHT dht(DHT_PIN, DHT_TYPE);

// FreeRTOS Handles
TaskHandle_t taskNetworkHandle = NULL;   // Core 0 – WiFi + MQTT
TaskHandle_t taskSensorHandle  = NULL;   // Core 1 – Lectura de DHT
TaskHandle_t taskPublishHandle = NULL;   // Core 1 – Publicador a RabbitMQ

// Cola FIFO: taskSensor envia datos -> taskPublish los saca y publica
QueueHandle_t sensorQueue;

// Mutex para proteger recursos compartidos
SemaphoreHandle_t wifiMutex;
SemaphoreHandle_t mqttMutex;

// Estado compartido (volatile por acceso multitarea)
volatile bool wifiConnected = false;
volatile bool mqttConnected = false;

// Contador de lecturas
int numLecturas = 0;

// Contador de lecturas consecutivas con valor anomalo
int lecturasInvalidasSeguidas = 0;

// ===========================================================================
// FUNCION AUXILIAR: Publicar error en topic de errores
// ===========================================================================

void publishError(const char* errorType, const char* message) {
  JsonDocument doc;
  doc["error_type"] = errorType;
  doc["message"]    = message;
  doc["esp_millis"] = millis();
  doc["free_heap"]  = ESP.getFreeHeap();

  char jsonBuffer[256];
  serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));

  xSemaphoreTake(mqttMutex, portMAX_DELAY);
  if (mqttConnected && mqttClient.connected()) {
    mqttClient.publish(TOPIC_ERROR, jsonBuffer);
  }
  xSemaphoreGive(mqttMutex);

  Serial.printf("[Error] %s: %s\n", errorType, message);
}

// ===========================================================================
// TAREA: NETWORK MANAGER  (Core 0)
// Mantiene WiFi y conexion MQTT a RabbitMQ
// ===========================================================================

void taskNetwork(void *parameter) {
  Serial.println("[Network] Iniciado en Core " + String(xPortGetCoreID()));
  unsigned long lastMqttAttempt = 0;

  // ── Conexion WiFi ──
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[Network] Conectando a WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }

  xSemaphoreTake(wifiMutex, portMAX_DELAY);
  wifiConnected = true;
  xSemaphoreGive(wifiMutex);

  Serial.println();
  Serial.println("[Network] WiFi conectado - IP: " + WiFi.localIP().toString());

  // ── Configurar MQTT (RabbitMQ) ──
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setBufferSize(512);
  mqttClient.setSocketTimeout(2);

  // ── Bucle principal: mantener conexiones vivas ──
  for (;;) {
    // Verificar WiFi
    if (WiFi.status() != WL_CONNECTED) {
      xSemaphoreTake(wifiMutex, portMAX_DELAY);
      wifiConnected = false;
      xSemaphoreGive(wifiMutex);

      Serial.println("[Network] WiFi perdido, reconectando...");
      WiFi.reconnect();

      int intentos = 0;
      while (WiFi.status() != WL_CONNECTED && intentos < 20) {
        vTaskDelay(500 / portTICK_PERIOD_MS);
        intentos++;
      }

      if (WiFi.status() == WL_CONNECTED) {
        xSemaphoreTake(wifiMutex, portMAX_DELAY);
        wifiConnected = true;
        xSemaphoreGive(wifiMutex);
        Serial.println("[Network] WiFi reconectado");
      } else {
        Serial.println("[Network] WiFi fallo, reintentando...");
        publishError("WIFI_RECONNECT_FAIL", "No se pudo reconectar a WiFi tras 20 intentos");
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        continue;
      }
    }

    // Verificar MQTT / RabbitMQ
    bool mqttIsConnected = false;
    xSemaphoreTake(mqttMutex, portMAX_DELAY);
    mqttIsConnected = mqttClient.connected();
    if (!mqttIsConnected) {
      mqttConnected = false;
    }
    xSemaphoreGive(mqttMutex);

    if (!mqttIsConnected && (millis() - lastMqttAttempt >= INTERVALO_REINTENTO_MQTT_MS)) {
      lastMqttAttempt = millis();
      Serial.println("[Network] Conectando a RabbitMQ (MQTT)...");

      String clientId = "ESP32-TempHum-" + String(random(0xFFFF), HEX);

      xSemaphoreTake(mqttMutex, portMAX_DELAY);
      bool connected = mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD);
      if (connected) {
        mqttConnected = true;
        Serial.println("[Network] RabbitMQ conectado");
        mqttClient.publish(TOPIC_STATUS, "{\"status\":\"online\",\"device\":\"ESP32-TempHum\"}", true);
      } else {
        mqttConnected = false;
        int rc = mqttClient.state();
        Serial.print("[Network] Error MQTT rc=");
        Serial.println(rc);
      }
      xSemaphoreGive(mqttMutex);

      // Cede CPU inmediatamente despues de un intento de conexion (exitoso o no).
      vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    xSemaphoreTake(mqttMutex, portMAX_DELAY);
    mqttClient.loop();
    xSemaphoreGive(mqttMutex);

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// ===========================================================================
// TAREA: SENSOR READER  (Core 1)
// Lee temperatura/humedad y envia datos a la cola
// ===========================================================================

void taskSensorReader(void *parameter) {
  Serial.println("[Sensor] Iniciado en Core " + String(xPortGetCoreID()));

  // Esperar a que la red este lista
  while (!wifiConnected || !mqttConnected) {
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }

  for (;;) {
    // Verificar conexiones
    xSemaphoreTake(wifiMutex, portMAX_DELAY);
    bool wifiOk = wifiConnected;
    xSemaphoreGive(wifiMutex);

    xSemaphoreTake(mqttMutex, portMAX_DELAY);
    bool mqttOk = mqttConnected;
    xSemaphoreGive(mqttMutex);

    if (!wifiOk || !mqttOk) {
      Serial.println("[Sensor] Esperando conexiones...");
      vTaskDelay(5000 / portTICK_PERIOD_MS);
      continue;
    }

    // ── Leer sensor DHT ──
    float humidity = dht.readHumidity();
    float temperatureC = dht.readTemperature();
    numLecturas++;

    // ── Deteccion de errores del sensor ──
    if (isnan(humidity) || isnan(temperatureC)) {
      lecturasInvalidasSeguidas++;
      if (lecturasInvalidasSeguidas >= 3) {
        publishError("DHT_READ_FAIL", "3 lecturas invalidas consecutivas del sensor DHT");
        lecturasInvalidasSeguidas = 0;
      }
      vTaskDelay(INTERVALO_LECTURA_MS / portTICK_PERIOD_MS);
      continue;
    }

    lecturasInvalidasSeguidas = 0;

    if (humidity < 0.0f || humidity > 100.0f || temperatureC < -40.0f || temperatureC > 125.0f) {
      publishError("SENSOR_OUT_OF_RANGE", "Lectura de temperatura/humedad fuera de rango esperado");
    }

    // Alerta de memoria baja (heap < 10 KB)
    if (ESP.getFreeHeap() < 10240) {
      publishError("LOW_MEMORY", "Heap libre menor a 10 KB");
    }

    // ── Llenar estructura (sin procesar) ──
    EnvData data;
    data.temperatureC  = temperatureC;
    data.humidity      = humidity;
    data.readingNumber = numLecturas;
    data.espMillis     = millis();

    Serial.println();
    Serial.println("==========================================");
    Serial.printf("[Sensor] Lectura #%d | Temp: %.2f C | Hum: %.2f %% | Millis: %lu\n",
                  data.readingNumber, data.temperatureC, data.humidity, data.espMillis);

    // Enviar a la cola para que el Publisher lo publique en RabbitMQ
    if (xQueueSend(sensorQueue, &data, pdMS_TO_TICKS(2000)) == pdTRUE) {
      Serial.println("[Sensor] Datos enviados a cola de publicacion");
    } else {
      Serial.println("[Sensor] Cola llena, dato descartado");
      publishError("QUEUE_FULL", "Cola de sensor llena, dato descartado");
    }

    vTaskDelay(INTERVALO_LECTURA_MS / portTICK_PERIOD_MS);
  }
}

// ===========================================================================
// TAREA: PUBLISHER  (Core 1)
// Lee datos de la cola y los publica a RabbitMQ via MQTT
// ===========================================================================

void taskPublisher(void *parameter) {
  Serial.println("[Publisher] Iniciado en Core " + String(xPortGetCoreID()));

  EnvData data;

  for (;;) {
    // Bloquea hasta que haya un elemento en la cola
    if (xQueueReceive(sensorQueue, &data, portMAX_DELAY) == pdTRUE) {

      // Serializar a JSON para consumo de subscribers
      JsonDocument doc;
      doc["reading"]       = data.readingNumber;
      doc["temperature_c"] = data.temperatureC;
      doc["humidity"]      = data.humidity;
      doc["esp_millis"]    = data.espMillis;

      char jsonBuffer[256];
      size_t len = serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));

      // Publicar usando mutex (mqttClient no es thread-safe)
      xSemaphoreTake(mqttMutex, portMAX_DELAY);
      bool ok = false;
      if (mqttConnected && mqttClient.connected()) {
        ok = mqttClient.publish(TOPIC_READING, jsonBuffer);
      }
      xSemaphoreGive(mqttMutex);

      if (ok) {
        Serial.printf("[Publisher] Publicado lectura #%d (%d bytes) en '%s'\n",
                      data.readingNumber, len, TOPIC_READING);
      } else {
        Serial.println("[Publisher] Error publicando en RabbitMQ");
        publishError("PUBLISH_FAIL", "No se pudo publicar lectura en RabbitMQ");
      }
    }
  }
}

// ===========================================================================
// SETUP
// ===========================================================================

void setup() {
  Serial.begin(9600);
  while (!Serial) { delay(10); }
  Serial.println();
  Serial.println("==========================================");
  Serial.println("  ESP32 - Sensor de Temperatura/Humedad");
  Serial.println("  RabbitMQ via MQTT | FreeRTOS Dual-Core");
  Serial.println("==========================================");

  randomSeed(analogRead(0));
  dht.begin();

  // Crear mutex
  wifiMutex = xSemaphoreCreateMutex();
  mqttMutex = xSemaphoreCreateMutex();

  // Cola de 10 elementos EnvData
  sensorQueue = xQueueCreate(10, sizeof(EnvData));

  // ── Core 0: Tarea de red (WiFi + RabbitMQ/MQTT) ──
  xTaskCreatePinnedToCore(
    taskNetwork,
    "TaskNetwork",
    8192,
    NULL,
    1,                    // Evita monopolizar CPU 0
    &taskNetworkHandle,
    0                     // Core 0
  );

  // ── Core 1: Tarea de lectura del sensor ──
  xTaskCreatePinnedToCore(
    taskSensorReader,
    "TaskSensor",
    4096,
    NULL,
    1,                    // Prioridad media
    &taskSensorHandle,
    1                     // Core 1
  );

  // ── Core 1: Tarea de publicacion a RabbitMQ ──
  xTaskCreatePinnedToCore(
    taskPublisher,
    "TaskPublisher",
    8192,
    NULL,
    1,                    // Misma prioridad que Sensor
    &taskPublishHandle,
    1                     // Core 1
  );

  Serial.println("[Setup] Tareas RTOS creadas – sistema en marcha");
}
void loop() {
  vTaskDelay(portMAX_DELAY);
}