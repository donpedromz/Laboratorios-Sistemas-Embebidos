#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <string.h>
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

// MQTT Topics para control de acceso
const char* TOPIC_STATUS         = "sensor/esp32/status";
const char* TOPIC_ERROR          = "sensor/esp32/error";
const char* TOPIC_COMMAND        = "auth/esp32/command";
const char* TOPIC_LOGIN_ATTEMPT  = "auth/login/attempt";
const char* TOPIC_AUTH_RESULT    = "auth/login/result";
const char* TOPIC_CONTROL_LOG    = "auth/esp32/log";
const char* DEVICE_ID            = "ESP32-ControlAcceso";

const int BUZZER_PIN = 18;
const int SERVO_PIN = 19;

const int SERVO_ANGLE_CERRADO = 0;
const int SERVO_ANGLE_ABIERTO = 90;
const int ALARMA_RETRY_THRESHOLD = 3;
const unsigned long ALARMA_SIRENA_TOTAL_MS = 30000;
const unsigned long ALARMA_BLOQUEO_TOTAL_MS = 60000;
const unsigned long ALARMA_SIRENA_PERIODO_MS = 800;
const unsigned long SAFEBOX_CLOSE_TIMEOUT_MS = 120000;
const unsigned long SAFEBOX_CLOSE_TIMEOUT_MAX_MS = 30UL * 60UL * 1000UL;
const int INTERVALO_REINTENTO_MQTT_MS = 5000;

const bool BUZZER_ACTIVE_LOW = true;
const uint8_t BUZZER_ON_LEVEL = BUZZER_ACTIVE_LOW ? LOW : HIGH;
const uint8_t BUZZER_OFF_LEVEL = BUZZER_ACTIVE_LOW ? HIGH : LOW;

// ===========================================================================
// ESTRUCTURAS
// ===========================================================================

struct MqttEvent {
  char topic[64];
  char payload[256];
  bool retained;
};

// ===========================================================================
// VARIABLES GLOBALES
// ===========================================================================

WiFiClient espClient;
PubSubClient mqttClient(espClient);
Servo lockServo;

TaskHandle_t taskNetworkHandle = NULL;
TaskHandle_t taskActuatorHandle = NULL;
TaskHandle_t taskSerialInputHandle = NULL;

QueueHandle_t mqttEventQueue = NULL;

SemaphoreHandle_t wifiMutex;
SemaphoreHandle_t mqttMutex;

volatile bool wifiConnected = false;
volatile bool mqttConnected = false;
volatile bool cerrojoAbierto = false;
volatile unsigned long servoAutoCloseAt = 0;
volatile bool buzzerActivo = false;
volatile unsigned long buzzerOffAt = 0;
volatile unsigned long buzzerToggleAt = 0;
volatile bool buzzerLevelOn = false;
volatile int failedAuthStreak = 0;

String serialBuffer = "";
String pendingUser = "";

enum SerialInputStep {
  WAIT_USERNAME,
  WAIT_PASSWORD,
  WAIT_AUTH_RESPONSE,
  AUTH_MENU,
};

volatile SerialInputStep serialStep = WAIT_USERNAME;

bool enqueueMqttEvent(const char* topic, const char* payload, bool retained);
void enqueueControlLog(const char* evento, const char* detalle);
void activarBuzzer(unsigned long duracionMs);
void desactivarBuzzer(const char* motivo);
void redirigirFlujoALogin(const char* motivo);
void cerrarCerrojoPorTimeout();

// ===========================================================================
// ESTADO SEGURO DE REINICIO
// ===========================================================================

void aplicarEstadoSeguroInicial(const char* motivo) {
  digitalWrite(BUZZER_PIN, BUZZER_OFF_LEVEL);
  buzzerActivo = false;
  buzzerOffAt = 0;
  buzzerToggleAt = 0;
  buzzerLevelOn = false;
  failedAuthStreak = 0;

  lockServo.write(SERVO_ANGLE_CERRADO);
  cerrojoAbierto = false;
  servoAutoCloseAt = 0;

  Serial.printf("[Init] Estado seguro aplicado (%s): SERVO=CERRADO, BUZZER=OFF\n", motivo);
}

void reportarEstadoReinicio(const char* motivo) {
  JsonDocument statusDoc;
  statusDoc["status"] = "restart_safe_state";
  statusDoc["device"] = DEVICE_ID;
  statusDoc["motivo"] = motivo;
  statusDoc["servo"] = "CERRADO";
  statusDoc["buzzer"] = "OFF";
  statusDoc["esp_millis"] = millis();

  char statusPayload[192];
  serializeJson(statusDoc, statusPayload, sizeof(statusPayload));
  enqueueMqttEvent(TOPIC_STATUS, statusPayload, true);

  enqueueControlLog("REINICIO_ESTADO_SEGURO", motivo);
}

// ===========================================================================
// COLA DE EVENTOS MQTT
// ===========================================================================

bool enqueueMqttEvent(const char* topic, const char* payload, bool retained = false) {
  if (mqttEventQueue == NULL || topic == NULL || payload == NULL) {
    return false;
  }

  MqttEvent ev;
  strncpy(ev.topic, topic, sizeof(ev.topic) - 1);
  ev.topic[sizeof(ev.topic) - 1] = '\0';

  strncpy(ev.payload, payload, sizeof(ev.payload) - 1);
  ev.payload[sizeof(ev.payload) - 1] = '\0';

  ev.retained = retained;

  BaseType_t sent = xQueueSend(mqttEventQueue, &ev, 0);
  if (sent != pdTRUE) {
    Serial.println("[MQTT] Cola de eventos llena, log descartado");
    return false;
  }

  return true;
}

void enqueueControlLog(const char* evento, const char* detalle) {
  JsonDocument doc;
  doc["evento"] = evento;
  doc["detalle"] = detalle;
  doc["device_id"] = DEVICE_ID;
  doc["esp_millis"] = millis();

  char payload[256];
  serializeJson(doc, payload, sizeof(payload));
  enqueueMqttEvent(TOPIC_CONTROL_LOG, payload, false);
}

void flushMqttEventQueue() {
  if (!mqttConnected || !mqttClient.connected() || mqttEventQueue == NULL) {
    return;
  }

  MqttEvent ev;
  while (xQueueReceive(mqttEventQueue, &ev, 0) == pdTRUE) {
    xSemaphoreTake(mqttMutex, portMAX_DELAY);
    bool ok = mqttClient.connected() && mqttClient.publish(ev.topic, ev.payload, ev.retained);
    xSemaphoreGive(mqttMutex);

    if (!ok) {
      // Si falla publicacion, intentamos devolverlo al frente para no perderlo.
      if (xQueueSendToFront(mqttEventQueue, &ev, 0) != pdTRUE) {
        Serial.println("[MQTT] No se pudo reencolar evento fallido");
      }
      break;
    }
  }
}

// ===========================================================================
// CONTROL DE ACTUADORES
// ===========================================================================

void abrirCerrojo(unsigned long duracionMs) {
  (void)duracionMs;

  unsigned long duracionAplicada = SAFEBOX_CLOSE_TIMEOUT_MS;
  if (duracionAplicada > SAFEBOX_CLOSE_TIMEOUT_MAX_MS) {
    duracionAplicada = SAFEBOX_CLOSE_TIMEOUT_MAX_MS;
  }

  lockServo.write(SERVO_ANGLE_ABIERTO);
  cerrojoAbierto = true;
  servoAutoCloseAt = millis() + duracionAplicada;
  Serial.printf("[Actuador] Cerrojo ABIERTO (%lums)\n", duracionAplicada);
  enqueueControlLog("CERROJO_ABIERTO", "Comando ABRIR_SEGURO ejecutado");
}

void cerrarCerrojo() {
  lockServo.write(SERVO_ANGLE_CERRADO);
  cerrojoAbierto = false;
  servoAutoCloseAt = 0;
  Serial.println("[Actuador] Cerrojo CERRADO");
  enqueueControlLog("CERROJO_CERRADO", "Comando CERRAR_SEGURO ejecutado");
}

void redirigirFlujoALogin(const char* motivo) {
  serialBuffer = "";
  pendingUser = "";
  serialStep = WAIT_USERNAME;

  if (motivo != NULL && strlen(motivo) > 0) {
    Serial.printf("[Auth] Flujo reiniciado: %s\n", motivo);
  }
  Serial.println("[Serial] Ingrese usuario:");
}

void cerrarCerrojoPorTimeout() {
  lockServo.write(SERVO_ANGLE_CERRADO);
  cerrojoAbierto = false;
  servoAutoCloseAt = 0;
  Serial.println("[Actuador] Cerrojo CERRADO (timeout)");
  enqueueControlLog("CERROJO_CERRADO_TIMEOUT", "Cierre automatico por timeout");
  redirigirFlujoALogin("Cierre automatico por timeout");
}

void activarBuzzer(unsigned long duracionMs) {
  const unsigned long duracionTotal = duracionMs > 0 ? duracionMs : ALARMA_SIRENA_TOTAL_MS;

  unsigned long periodo = ALARMA_SIRENA_PERIODO_MS;
  if (periodo < 100) {
    periodo = 100;
  }
  unsigned long medioPeriodo = periodo / 2;
  if (medioPeriodo == 0) {
    medioPeriodo = 1;
  }

  digitalWrite(BUZZER_PIN, BUZZER_ON_LEVEL);
  buzzerLevelOn = true;
  buzzerActivo = true;
  buzzerOffAt = millis() + duracionTotal;
  buzzerToggleAt = millis() + medioPeriodo;
  Serial.printf("[Actuador] Sirena ON (total=%lums, periodo=%lums)\n", duracionTotal, periodo);
  enqueueControlLog("BUZZER_ACTIVO", "Comando ENCENDER_ZUMBADOR ejecutado");
}

void desactivarBuzzer(const char* motivo) {
  if (!buzzerActivo && !buzzerLevelOn) {
    return;
  }

  digitalWrite(BUZZER_PIN, BUZZER_OFF_LEVEL);
  buzzerActivo = false;
  buzzerLevelOn = false;
  buzzerOffAt = 0;
  buzzerToggleAt = 0;
  Serial.printf("[Actuador] Sirena OFF (%s)\n", motivo);
  enqueueControlLog("BUZZER_APAGADO", motivo);
}

// ===========================================================================
// FUNCIONES MQTT
// ===========================================================================

void publishError(const char* errorType, const char* message) {
  JsonDocument doc;
  doc["error_type"] = errorType;
  doc["message"]    = message;
  doc["esp_millis"] = millis();
  doc["free_heap"]  = ESP.getFreeHeap();

  char payload[256];
  serializeJson(doc, payload, sizeof(payload));
  enqueueMqttEvent(TOPIC_ERROR, payload, false);

  Serial.printf("[Error] %s: %s\n", errorType, message);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, TOPIC_AUTH_RESULT) == 0) {
    if (length == 0 || length > 384) {
      Serial.println("[MQTT] Resultado auth invalido");
      return;
    }

    char jsonBuffer[385];
    memcpy(jsonBuffer, payload, length);
    jsonBuffer[length] = '\0';

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, jsonBuffer);
    if (err) {
      Serial.printf("[MQTT] Error parseando resultado auth: %s\n", err.c_str());
      return;
    }

    const bool autenticado = doc["autenticado"] | false;
    const bool bloqueado = doc["bloqueado"] | false;
    const int intentosFallidos = doc["intentos_fallidos"] | 0;
    const int reintentosMaximos = doc["reintentos_maximos"] | ALARMA_RETRY_THRESHOLD;
    const int reintentosDisponibles = doc["reintentos_disponibles"] | max(reintentosMaximos - intentosFallidos, 0);
    const char* detalle = doc["detalle"] | "sin_detalle";
    Serial.printf("[Auth] Resultado: %s (%s)\n", autenticado ? "OK" : "FALLO", detalle);
    enqueueControlLog(autenticado ? "AUTH_OK" : "AUTH_FALLO", detalle);

    pendingUser = "";
    serialBuffer = "";
    if (autenticado) {
      failedAuthStreak = 0;
      desactivarBuzzer("Credenciales correctas");
      serialStep = AUTH_MENU;
      Serial.println("\n[Menu] Acceso autorizado.");
      Serial.println("[Menu] 1 - Cerrar caja fuerte");
      Serial.println("[Menu] Ingrese opcion:");
    } else {
      Serial.printf("[Auth] Reintentos disponibles antes de bloqueo: %d/%d\n", reintentosDisponibles, reintentosMaximos);
      failedAuthStreak = intentosFallidos > 0 ? intentosFallidos : (failedAuthStreak + 1);
      if (bloqueado) {
        activarBuzzer(ALARMA_BLOQUEO_TOTAL_MS);
      } else if (failedAuthStreak >= ALARMA_RETRY_THRESHOLD) {
        activarBuzzer(ALARMA_SIRENA_TOTAL_MS);
      }

      serialStep = WAIT_USERNAME;
      Serial.println("\n[Serial] Ingrese usuario:");
    }
    return;
  }

  if (strcmp(topic, TOPIC_COMMAND) != 0) {
    return;
  }

  if (length == 0 || length > 384) {
    Serial.println("[MQTT] Payload de comando vacio o demasiado grande");
    return;
  }

  char jsonBuffer[385];
  memcpy(jsonBuffer, payload, length);
  jsonBuffer[length] = '\0';

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, jsonBuffer);
  if (err) {
    Serial.printf("[MQTT] Error parseando comando JSON: %s\n", err.c_str());
    return;
  }

  const char* accion = doc["accion"] | "";

  Serial.printf("[MQTT] Comando recibido: %s\n", accion);

  if (strcmp(accion, "ABRIR_SEGURO") == 0) {
    abrirCerrojo(0);
    return;
  }

  if (strcmp(accion, "CERRAR_SEGURO") == 0) {
    cerrarCerrojo();
    return;
  }

  if (strcmp(accion, "ENCENDER_ZUMBADOR") == 0) {
    const unsigned long duracionMs = doc["duracion_ms"] | 0;
    activarBuzzer(duracionMs);
    return;
  }

  Serial.printf("[MQTT] Accion no soportada: %s\n", accion);
  enqueueControlLog("COMANDO_DESCONOCIDO", accion);
}

bool publishLoginAttempt(const String& usuario, const String& contrasena) {
  JsonDocument doc;
  doc["usuario"] = usuario;
  doc["contrasena"] = contrasena;
  doc["dispositivo_id"] = DEVICE_ID;
  doc["esp_millis"] = millis();

  char payload[256];
  serializeJson(doc, payload, sizeof(payload));

  xSemaphoreTake(mqttMutex, portMAX_DELAY);
  bool ok = false;
  if (mqttConnected && mqttClient.connected()) {
    ok = mqttClient.publish(TOPIC_LOGIN_ATTEMPT, payload, false);
  }
  xSemaphoreGive(mqttMutex);

  Serial.printf("[Auth] Intento enviado para usuario '%s': %s\n", usuario.c_str(), ok ? "OK" : "FALLO");
  enqueueControlLog(ok ? "LOGIN_INTENTO_ENVIADO" : "LOGIN_INTENTO_FALLIDO", usuario.c_str());
  return ok;
}

// ===========================================================================
// TAREAS
// ===========================================================================

void taskActuators(void *parameter) {
  Serial.println("[Actuator] Iniciado en Core " + String(xPortGetCoreID()));

  for (;;) {
    const unsigned long now = millis();

    if (buzzerActivo) {
      if ((long)(now - buzzerOffAt) >= 0) {
        desactivarBuzzer("Sirena finalizada por timeout");
      } else if ((long)(now - buzzerToggleAt) >= 0) {
        buzzerLevelOn = !buzzerLevelOn;
        digitalWrite(BUZZER_PIN, buzzerLevelOn ? BUZZER_ON_LEVEL : BUZZER_OFF_LEVEL);

        unsigned long medioPeriodo = ALARMA_SIRENA_PERIODO_MS / 2;
        if (medioPeriodo == 0) {
          medioPeriodo = 1;
        }
        buzzerToggleAt = now + medioPeriodo;
      }
    }

    if (cerrojoAbierto && servoAutoCloseAt > 0 && (long)(now - servoAutoCloseAt) >= 0) {
      cerrarCerrojoPorTimeout();
    }

    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}

void taskSerialInput(void *parameter) {
  Serial.println("[Serial] Ingrese usuario:");

  for (;;) {
    while (Serial.available() > 0) {
      char c = static_cast<char>(Serial.read());

      if (c == '\r') {
        continue;
      }

      if (c != '\n') {
        serialBuffer += c;
        continue;
      }

      serialBuffer.trim();

      if (serialStep == WAIT_USERNAME) {
        if (serialBuffer.length() == 0) {
          Serial.println("[Serial] Usuario vacio. Ingrese usuario:");
          serialBuffer = "";
          continue;
        }

        pendingUser = serialBuffer;
        serialBuffer = "";
        serialStep = WAIT_PASSWORD;
        Serial.println("[Serial] Ingrese contrasena:");
        continue;
      }

      if (serialStep == WAIT_PASSWORD) {
        if (serialBuffer.length() == 0) {
          Serial.println("[Serial] Contrasena vacia. Ingrese contrasena:");
          serialBuffer = "";
          continue;
        }

        if (!mqttConnected || !wifiConnected) {
          Serial.println("[Auth] Sin conexion MQTT/WiFi. Reintente en unos segundos.");
          serialBuffer = "";
          serialStep = WAIT_USERNAME;
          Serial.println("[Serial] Ingrese usuario:");
          continue;
        }

        bool envioOk = publishLoginAttempt(pendingUser, serialBuffer);
        serialBuffer = "";
        if (envioOk) {
          serialStep = WAIT_AUTH_RESPONSE;
          Serial.println("[Auth] Esperando respuesta de autenticacion...");
        } else {
          pendingUser = "";
          serialStep = WAIT_USERNAME;
          Serial.println("[Serial] Ingrese usuario:");
        }
        continue;
      }

      if (serialStep == WAIT_AUTH_RESPONSE) {
        serialBuffer = "";
        Serial.println("[Auth] Aun esperando respuesta. Espere unos segundos...");
        continue;
      }

      if (serialStep == AUTH_MENU) {
        if (serialBuffer == "1") {
          cerrarCerrojo();
          enqueueControlLog("CIERRE_MANUAL", "Cierre solicitado desde menu serial");
          serialBuffer = "";
          pendingUser = "";
          serialStep = WAIT_USERNAME;
          Serial.println("[Menu] Caja fuerte cerrada.");
          Serial.println("[Serial] Ingrese usuario:");
          continue;
        }

        serialBuffer = "";
        Serial.println("[Menu] Opcion invalida.");
        Serial.println("[Menu] 1 - Cerrar caja fuerte");
        Serial.println("[Menu] Ingrese opcion:");
      }
    }

    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}

void taskNetwork(void *parameter) {
  Serial.println("[Network] Iniciado en Core " + String(xPortGetCoreID()));
  unsigned long lastMqttAttempt = 0;

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

  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setBufferSize(512);
  mqttClient.setSocketTimeout(2);
  mqttClient.setCallback(mqttCallback);

  for (;;) {
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

      String clientId = "ESP32-ControlAcceso-" + String(random(0xFFFF), HEX);

      xSemaphoreTake(mqttMutex, portMAX_DELAY);
      bool connected = mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD);
      if (connected) {
        mqttConnected = true;
        Serial.println("[Network] RabbitMQ conectado");

        aplicarEstadoSeguroInicial("mqtt_reconnect");
        reportarEstadoReinicio("mqtt_reconnect");

        mqttClient.subscribe(TOPIC_COMMAND, 1);
        mqttClient.subscribe(TOPIC_AUTH_RESULT, 1);
        Serial.printf("[Network] Suscrito a comandos: %s\n", TOPIC_COMMAND);
        Serial.printf("[Network] Suscrito a resultado auth: %s\n", TOPIC_AUTH_RESULT);

        JsonDocument onlineDoc;
        onlineDoc["status"] = "online";
        onlineDoc["device"] = DEVICE_ID;
        onlineDoc["esp_millis"] = millis();
        char onlinePayload[128];
        serializeJson(onlineDoc, onlinePayload, sizeof(onlinePayload));
        mqttClient.publish(TOPIC_STATUS, onlinePayload, true);
      } else {
        mqttConnected = false;
        int rc = mqttClient.state();
        Serial.print("[Network] Error MQTT rc=");
        Serial.println(rc);
      }
      xSemaphoreGive(mqttMutex);

      vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    xSemaphoreTake(mqttMutex, portMAX_DELAY);
    mqttClient.loop();
    xSemaphoreGive(mqttMutex);

    flushMqttEventQueue();

    vTaskDelay(1000 / portTICK_PERIOD_MS);
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
  Serial.println("  ESP32 - Control de Acceso");
  Serial.println("  RabbitMQ via MQTT | FreeRTOS Dual-Core");
  Serial.println("==========================================");

  randomSeed(analogRead(0));

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, BUZZER_OFF_LEVEL);

  lockServo.setPeriodHertz(50);
  lockServo.attach(SERVO_PIN, 500, 2400);
  aplicarEstadoSeguroInicial("boot");

  wifiMutex = xSemaphoreCreateMutex();
  mqttMutex = xSemaphoreCreateMutex();
  mqttEventQueue = xQueueCreate(20, sizeof(MqttEvent));

  if (wifiMutex == NULL || mqttMutex == NULL || mqttEventQueue == NULL) {
    Serial.println("[Setup] Error creando mutex/queue de FreeRTOS");
  }

  reportarEstadoReinicio("boot");

  xTaskCreatePinnedToCore(
    taskNetwork,
    "TaskNetwork",
    8192,
    NULL,
    1,
    &taskNetworkHandle,
    0
  );

  xTaskCreatePinnedToCore(
    taskActuators,
    "TaskActuator",
    4096,
    NULL,
    1,
    &taskActuatorHandle,
    1
  );

  xTaskCreatePinnedToCore(
    taskSerialInput,
    "TaskSerialInput",
    4096,
    NULL,
    1,
    &taskSerialInputHandle,
    1
  );

  Serial.println("[Setup] Tareas RTOS creadas - sistema en marcha");
}

void loop() {
  vTaskDelay(portMAX_DELAY);
}
