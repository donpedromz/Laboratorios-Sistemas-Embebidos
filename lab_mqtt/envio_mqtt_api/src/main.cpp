#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// ===========================================================================
// CONFIGURACION
// ===========================================================================
const char* WIFI_SSID     = "Redmi Note 10 Pro";
const char* WIFI_PASSWORD = "perrito123";

// RabbitMQ via plugin MQTT (puerto 1883)
const char* MQTT_SERVER   = "10.46.80.245";
const int   MQTT_PORT     = 1883;
const char* MQTT_USER     = "iot_device";
const char* MQTT_PASSWORD = "SecurePass123!";

// User-Agent requerido por Nominatim (politica de uso)
const char* USER_AGENT = "ESP32-Manizales-Walker/1.0 (donpedromz@labs.com)";

// MQTT Topics para RabbitMQ
const char* TOPIC_LOCATION = "location/manizales/current";
const char* TOPIC_STATUS   = "location/esp32/status";
const char* TOPIC_ERROR    = "location/esp32/error";

// ===========================================================================
// ESTRUCTURA DE DATOS - Nominatim
// ===========================================================================

struct NominatimData {
  double latitude;            // Latitud consultada
  double longitude;           // Longitud consultada
  char   road[80];            // Calle / via
  char   neighbourhood[80];   // Barrio
  char   suburb[80];          // Suburbio / comuna
  char   city[50];            // Ciudad
  char   type[40];            // Tipo de lugar (residential, tertiary…)
  char   displayName[200];    // Nombre completo devuelto por Nominatim
  int    stepNumber;          // Numero de paso en la caminata
  double distFromOrigin;      // Distancia al origen en metros
  unsigned long espMillis;    // Timestamp interno del ESP32
};

// ===========================================================================
// VARIABLES GLOBALES
// ===========================================================================

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// FreeRTOS Handles
TaskHandle_t taskNetworkHandle  = NULL;   // Core 0 – WiFi + MQTT
TaskHandle_t taskNominatimHandle = NULL;  // Core 1 – Simulacion + API
TaskHandle_t taskPublishHandle  = NULL;   // Core 1 – Publicador a RabbitMQ

// Cola FIFO: taskNominatim envia datos -> taskPublish los saca y publica
QueueHandle_t nominatimQueue;

// Mutex para proteger recursos compartidos
SemaphoreHandle_t wifiMutex;
SemaphoreHandle_t mqttMutex;

// Estado compartido (volatile por acceso multitarea)
volatile bool wifiConnected = false;
volatile bool mqttConnected = false;

// ===========================================================================
// PARAMETROS DEL SIMULADOR DE CAMINATA
// ===========================================================================

const int    INTERVALO_MS   = 5000;   // Delay entre pasos (min 1100 por Nominatim)
const double PASO_METROS    = 50.0;   // Distancia de cada "paso" en metros
const double LIMITE_RADIO_M = 2000.0; // Radio maximo permitido desde el origen (2 km)

// Punto de inicio: Plaza de Bolivar, Manizales
const double LAT_ORIGEN = 5.06889;
const double LON_ORIGEN = -75.51738;

// Estado del simulador
double latActual = LAT_ORIGEN;
double lonActual = LON_ORIGEN;
int    numPasos  = 0;

// ===========================================================================
// FUNCIONES DE MOVIMIENTO
// ===========================================================================

double metrosAGradosLat(double metros) {
  return metros / 111320.0;
}

double metrosAGradosLon(double metros, double lat) {
  return metros / (111320.0 * cos(lat * DEG_TO_RAD));
}

double distanciaMetros(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371000.0;
  double phi1    = lat1 * DEG_TO_RAD;
  double phi2    = lat2 * DEG_TO_RAD;
  double dphi    = (lat2 - lat1) * DEG_TO_RAD;
  double dlambda = (lon2 - lon1) * DEG_TO_RAD;
  double a = sin(dphi / 2) * sin(dphi / 2)
           + cos(phi1) * cos(phi2) * sin(dlambda / 2) * sin(dlambda / 2);
  return R * 2 * atan2(sqrt(a), sqrt(1 - a));
}

void siguientePaso(double &lat, double &lon) {
  double dist = distanciaMetros(lat, lon, LAT_ORIGEN, LON_ORIGEN);
  double angulo;

  if (dist >= LIMITE_RADIO_M) {
    double dLat = LAT_ORIGEN - lat;
    double dLon = LON_ORIGEN - lon;
    double anguloBase = atan2(dLon, dLat) * RAD_TO_DEG;
    angulo = anguloBase + random(-45, 45);
  } else {
    angulo = random(0, 360);
  }

  double anguloRad = angulo * DEG_TO_RAD;
  double deltaLat = metrosAGradosLat(PASO_METROS * cos(anguloRad));
  double deltaLon = metrosAGradosLon(PASO_METROS * sin(anguloRad), lat);

  lat += deltaLat;
  lon += deltaLon;
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
  mqttClient.setBufferSize(1024);  // Permite mensajes JSON grandes

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
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        continue;
      }
    }

    // Verificar MQTT / RabbitMQ
    xSemaphoreTake(mqttMutex, portMAX_DELAY);
    if (!mqttClient.connected()) {
      mqttConnected = false;
      Serial.println("[Network] Conectando a RabbitMQ (MQTT)...");

      String clientId = "ESP32-Walker-" + String(random(0xFFFF), HEX);
      if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD)) {
        mqttConnected = true;
        Serial.println("[Network] RabbitMQ conectado");
        mqttClient.publish(TOPIC_STATUS, "{\"status\":\"online\",\"device\":\"ESP32-Walker\"}", true);
      } else {
        Serial.print("[Network] Error MQTT rc=");
        Serial.println(mqttClient.state());
      }
    }
    mqttClient.loop();
    xSemaphoreGive(mqttMutex);

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// ===========================================================================
// TAREA: NOMINATIM FETCHER  (Core 1)
// Simula movimiento y consulta Nominatim reverse-geocoding
// ===========================================================================

void taskNominatimFetcher(void *parameter) {
  Serial.println("[Nominatim] Iniciado en Core " + String(xPortGetCoreID()));

  // Esperar a que la red este lista
  while (!wifiConnected || !mqttConnected) {
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }

  HTTPClient http;

  for (;;) {
    // Verificar conexiones
    xSemaphoreTake(wifiMutex, portMAX_DELAY);
    bool wifiOk = wifiConnected;
    xSemaphoreGive(wifiMutex);

    xSemaphoreTake(mqttMutex, portMAX_DELAY);
    bool mqttOk = mqttConnected;
    xSemaphoreGive(mqttMutex);

    if (!wifiOk || !mqttOk) {
      Serial.println("[Nominatim] Esperando conexiones...");
      vTaskDelay(5000 / portTICK_PERIOD_MS);
      continue;
    }

    // ── Simular movimiento ──
    siguientePaso(latActual, lonActual);
    numPasos++;
    double distOrigen = distanciaMetros(latActual, lonActual, LAT_ORIGEN, LON_ORIGEN);

    Serial.println();
    Serial.println("==========================================");
    Serial.printf("[Nominatim] Paso #%d | Lat: %.6f | Lon: %.6f | Dist: %.0f m\n",
                  numPasos, latActual, lonActual, distOrigen);

    // ── Consultar Nominatim ──
    String url = "https://nominatim.openstreetmap.org/reverse"
                 "?lat=" + String(latActual, 6) +
                 "&lon=" + String(lonActual, 6) +
                 "&format=jsonv2&addressdetails=1&zoom=18";

    http.begin(url);
    http.addHeader("User-Agent", USER_AGENT);
    http.addHeader("Accept", "application/json");
    http.setTimeout(15000);

    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      Serial.printf("[Nominatim] Respuesta: %d bytes\n", payload.length());

      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, payload);

      if (err) {
        Serial.printf("[Nominatim] JSON Error: %s\n", err.c_str());
        http.end();
        vTaskDelay(INTERVALO_MS / portTICK_PERIOD_MS);
        continue;
      }

      // ── Llenar estructura ──
      NominatimData data;
      memset(&data, 0, sizeof(data));

      data.latitude       = latActual;
      data.longitude      = lonActual;
      data.stepNumber     = numPasos;
      data.distFromOrigin = distOrigen;
      data.espMillis      = millis();

      JsonObject addr = doc["address"];
      const char* tmp;

      tmp = addr["road"] | addr["pedestrian"] | "(sin nombre)";
      strncpy(data.road, tmp, sizeof(data.road) - 1);

      tmp = addr["neighbourhood"] | "";
      strncpy(data.neighbourhood, tmp, sizeof(data.neighbourhood) - 1);

      tmp = addr["suburb"] | "";
      strncpy(data.suburb, tmp, sizeof(data.suburb) - 1);

      tmp = addr["city"] | addr["town"] | addr["village"] | "";
      strncpy(data.city, tmp, sizeof(data.city) - 1);

      tmp = doc["type"] | "?";
      strncpy(data.type, tmp, sizeof(data.type) - 1);

      tmp = doc["display_name"] | "";
      strncpy(data.displayName, tmp, sizeof(data.displayName) - 1);

      // Imprimir resumen en Serial
      Serial.println("  ┌────────────────────────────────────────");
      Serial.printf("  | Calle: %s\n", data.road);
      Serial.printf("  | Barrio: %s\n", data.neighbourhood);
      Serial.printf("  | Comuna: %s\n", data.suburb);
      Serial.printf("  | Ciudad: %s\n", data.city);
      Serial.printf("  | Tipo: %s\n", data.type);
      Serial.println("  └────────────────────────────────────────");

      // Enviar a la cola para que el Publisher lo publique en RabbitMQ
      if (xQueueSend(nominatimQueue, &data, pdMS_TO_TICKS(2000)) == pdTRUE) {
        Serial.println("[Nominatim] Datos enviados a cola de publicacion");
      } else {
        Serial.println("[Nominatim] Cola llena, dato descartado");
      }

    } else {
      Serial.printf("[Nominatim] Error HTTP: %d\n", httpCode);

      xSemaphoreTake(mqttMutex, portMAX_DELAY);
      String errorMsg = "{\"error\":\"HTTP " + String(httpCode) + "\",\"step\":" + String(numPasos) + "}";
      mqttClient.publish(TOPIC_ERROR, errorMsg.c_str());
      xSemaphoreGive(mqttMutex);
    }

    http.end();

    // Respetar rate-limit de Nominatim (1 req/s – usamos 5 s de margen)
    Serial.printf("[Nominatim] Proximo paso en %d ms...\n", INTERVALO_MS);
    vTaskDelay(INTERVALO_MS / portTICK_PERIOD_MS);
  }
}

// ===========================================================================
// TAREA: PUBLISHER  (Core 1)
// Lee datos de la cola y los publica a RabbitMQ via MQTT
// ===========================================================================

void taskPublisher(void *parameter) {
  Serial.println("[Publisher] Iniciado en Core " + String(xPortGetCoreID()));

  NominatimData data;

  for (;;) {
    // Bloquea hasta que haya un elemento en la cola
    if (xQueueReceive(nominatimQueue, &data, portMAX_DELAY) == pdTRUE) {

      // Serializar a JSON para publicar en RabbitMQ
      JsonDocument doc;
      doc["step"]           = data.stepNumber;
      doc["lat"]            = serialized(String(data.latitude, 6));
      doc["lon"]            = serialized(String(data.longitude, 6));
      doc["road"]           = data.road;
      doc["neighbourhood"]  = data.neighbourhood;
      doc["suburb"]         = data.suburb;
      doc["city"]           = data.city;
      doc["type"]           = data.type;
      doc["display_name"]   = data.displayName;
      doc["dist_origin_m"]  = (int)data.distFromOrigin;
      doc["esp_millis"]     = data.espMillis;

      char jsonBuffer[512];
      size_t len = serializeJson(doc, jsonBuffer, sizeof(jsonBuffer));

      // Publicar usando mutex (mqttClient no es thread-safe)
      xSemaphoreTake(mqttMutex, portMAX_DELAY);
      bool ok = false;
      if (mqttConnected && mqttClient.connected()) {
        ok = mqttClient.publish(TOPIC_LOCATION, jsonBuffer);
      }
      xSemaphoreGive(mqttMutex);

      if (ok) {
        Serial.printf("[Publisher] Publicado paso #%d (%d bytes) en '%s'\n",
                      data.stepNumber, len, TOPIC_LOCATION);
      } else {
        Serial.println("[Publisher] Error publicando en RabbitMQ");
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
  Serial.println("  ESP32 - Caminata Manizales + Nominatim");
  Serial.println("  RabbitMQ via MQTT | FreeRTOS Dual-Core");
  Serial.println("==========================================");

  // Semilla para random walk
  randomSeed(analogRead(0));

  // Crear mutex
  wifiMutex = xSemaphoreCreateMutex();
  mqttMutex = xSemaphoreCreateMutex();

  // Cola de 5 elementos NominatimData
  nominatimQueue = xQueueCreate(5, sizeof(NominatimData));

  // ── Core 0: Tarea de red (WiFi + RabbitMQ/MQTT) ──
  xTaskCreatePinnedToCore(
    taskNetwork,          // Funcion de la tarea
    "TaskNetwork",        // Nombre
    8192,                 // Stack (bytes)
    NULL,                 // Parametro
    2,                    // Prioridad (alta – red es critica)
    &taskNetworkHandle,   // Handle
    0                     // Core 0
  );

  // ── Core 1: Tarea de consulta Nominatim (simulacion + API) ──
  xTaskCreatePinnedToCore(
    taskNominatimFetcher,
    "TaskNominatim",
    10240,
    NULL,
    1,                      // Prioridad media
    &taskNominatimHandle,
    1                       // Core 1
  );

  // ── Core 1: Tarea de publicacion a RabbitMQ ──
  xTaskCreatePinnedToCore(
    taskPublisher,
    "TaskPublisher",
    8192,
    NULL,
    1,                    // Misma prioridad que Nominatim
    &taskPublishHandle,
    1                     // Core 1
  );

  Serial.println("[Setup] Tareas RTOS creadas – sistema en marcha");
}

// ===========================================================================
// LOOP (vacio – todo corre en tareas FreeRTOS)
// ===========================================================================

void loop() {
  vTaskDelay(portMAX_DELAY);
}