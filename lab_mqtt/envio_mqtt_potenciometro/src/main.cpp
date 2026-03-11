#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
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
const char* TOPIC_VOLUME = "sensor/potenciometro/volume";
const char* TOPIC_STATUS = "sensor/esp32/status";
const char* TOPIC_ERROR  = "sensor/esp32/error";

// Pin del potenciometro 
const int POTEN_PIN = 34;

// Intervalo de lectura del sensor (ms)
const int INTERVALO_LECTURA_MS = 2000;

// ===========================================================================
// ESTRUCTURA DE DATOS - Lectura del potenciometro
// ===========================================================================

struct PotData {
  int    rawValue;         // Valor crudo del ADC (0-4095)
  int    readingNumber;    // Numero de lectura secuencial
  unsigned long espMillis;  // Timestamp interno del ESP32
};

// ===========================================================================
// VARIABLES GLOBALES
// ===========================================================================

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// FreeRTOS Handles
TaskHandle_t taskNetworkHandle = NULL;   // Core 0 – WiFi + MQTT
TaskHandle_t taskSensorHandle  = NULL;   // Core 1 – Lectura del potenciometro
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
int lecturasAnomalasSeguidas = 0;

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
    xSemaphoreTake(mqttMutex, portMAX_DELAY);
    if (!mqttClient.connected()) {
      mqttConnected = false;
      Serial.println("[Network] Conectando a RabbitMQ (MQTT)...");

      String clientId = "ESP32-Poten-" + String(random(0xFFFF), HEX);
      if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD)) {
        mqttConnected = true;
        Serial.println("[Network] RabbitMQ conectado");
        mqttClient.publish(TOPIC_STATUS, "{\"status\":\"online\",\"device\":\"ESP32-Potenciometro\"}", true);
      } else {
        int rc = mqttClient.state();
        Serial.print("[Network] Error MQTT rc=");
        Serial.println(rc);
        char msg[64];
        snprintf(msg, sizeof(msg), "MQTT connect failed rc=%d", rc);
        // Publicar error solo si hay WiFi (sin mutex, ya lo tenemos)
        if (wifiConnected) {
          mqttClient.publish(TOPIC_ERROR, msg);
        }
      }
    }
    mqttClient.loop();
    xSemaphoreGive(mqttMutex);

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// ===========================================================================
// TAREA: SENSOR READER  (Core 1)
// Lee el potenciometro y envia datos a la cola
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

    // ── Leer potenciometro (solo dato crudo) ──
    int rawValue = analogRead(POTEN_PIN);
    numLecturas++;

    // ── Deteccion de errores del sensor ──

    // Error: lectura siempre 0 o siempre 4095 (pin desconectado / corto)
    if (rawValue == 0 || rawValue == 4095) {
      lecturasAnomalasSeguidas++;
      if (lecturasAnomalasSeguidas == 5) {
        char msg[80];
        snprintf(msg, sizeof(msg),
                 "Pin %d: 5 lecturas consecutivas en %d (posible pin desconectado o en corto)",
                 POTEN_PIN, rawValue);
        publishError("ADC_PIN_ANOMALY", msg);
      }
    } else {
      lecturasAnomalasSeguidas = 0;
    }

    // Alerta de memoria baja (heap < 10 KB)
    if (ESP.getFreeHeap() < 10240) {
      publishError("LOW_MEMORY", "Heap libre menor a 10 KB");
    }

    // ── Llenar estructura (sin procesar) ──
    PotData data;
    data.rawValue      = rawValue;
    data.readingNumber = numLecturas;
    data.espMillis     = millis();

    Serial.println();
    Serial.println("==========================================");
    Serial.printf("[Sensor] Lectura #%d | ADC Raw: %d | Millis: %lu\n",
                  data.readingNumber, data.rawValue, data.espMillis);

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

  PotData data;

  for (;;) {
    // Bloquea hasta que haya un elemento en la cola
    if (xQueueReceive(sensorQueue, &data, portMAX_DELAY) == pdTRUE) {

      // Serializar a JSON (solo datos crudos, el subscriber procesa)
      JsonDocument doc;
      doc["reading"]    = data.readingNumber;
      doc["raw"]        = data.rawValue;
      doc["esp_millis"] = data.espMillis;

      char jsonBuffer[256];
      size_t len = serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));

      // Publicar usando mutex (mqttClient no es thread-safe)
      xSemaphoreTake(mqttMutex, portMAX_DELAY);
      bool ok = false;
      if (mqttConnected && mqttClient.connected()) {
        ok = mqttClient.publish(TOPIC_VOLUME, jsonBuffer);
      }
      xSemaphoreGive(mqttMutex);

      if (ok) {
        Serial.printf("[Publisher] Publicado lectura #%d (%d bytes) en '%s'\n",
                      data.readingNumber, len, TOPIC_VOLUME);
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
  Serial.println("  ESP32 - Potenciometro (Volumen)");
  Serial.println("  RabbitMQ via MQTT | FreeRTOS Dual-Core");
  Serial.println("==========================================");

  randomSeed(analogRead(0));

  // Crear mutex
  wifiMutex = xSemaphoreCreateMutex();
  mqttMutex = xSemaphoreCreateMutex();

  // Cola de 10 elementos PotData
  sensorQueue = xQueueCreate(10, sizeof(PotData));

  // ── Core 0: Tarea de red (WiFi + RabbitMQ/MQTT) ──
  xTaskCreatePinnedToCore(
    taskNetwork,
    "TaskNetwork",
    8192,
    NULL,
    2,                    // Prioridad alta – red es critica
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