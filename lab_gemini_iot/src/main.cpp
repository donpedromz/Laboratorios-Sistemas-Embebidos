#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

/**
 * Configuracion central del sistema.
 */
namespace Config {
  /**
   * Pin del LED integrado
   */
  constexpr uint8_t kLedPin = 2;

  /**
   * Parametros del puerto serial
   */
  constexpr uint32_t kSerialBaudRate = 9600;

  /**
   * Credenciales y API key de Gemini
   */
#ifndef WIFI_SSID
  #define WIFI_SSID ""
  #define WIFI_PASS ""
#endif
#ifndef API_KEY
  #define API_KEY ""
  #define API_MODEL "gemini-3-flash-preview"
#endif

  constexpr const char* kWifiSsid = WIFI_SSID;
  constexpr const char* kWifiPassword = WIFI_PASS;
  constexpr const char* kGeminiToken = API_KEY;
  constexpr const char* kGeminiModel = API_MODEL;

  /**
   * Configuraciones de generación de Gemini
   */
  constexpr int kGeminiMaxTokens = 256;
  constexpr float kGeminiTemperature = 0.7; // Un poco más alto para humor

  /**
   * Instrucción del protocolo para juzgar chistes
   */
  constexpr const char* kGeminiSystemInstruction =
    "Eres un juez de chistes. Responda siempre con un JSON valido y nada mas. "
    "Formato: {\"respuesta\": \"Su opinion corta\", \"turn_led\": true|false}. "
    "Si el chiste es gracioso, turn_led debe ser true. Si no, false.";
}

String serialBuffer;

/**
 * Estructura que representa la respuesta de Gemini
 */
struct GeminiDecision {
  String respuesta;
  bool turnLed;
};

/**
 * Parsea el JSON de Gemini
 */
bool parseGeminiDecision(const String& rawJsonText, GeminiDecision& decision) {
  // Limpiar posibles bloques de markdown
  String jsonText = rawJsonText;
  jsonText.trim();
  if (jsonText.startsWith("```json")) {
    jsonText = jsonText.substring(7);
  }
  if (jsonText.endsWith("```")) {
    jsonText = jsonText.substring(0, jsonText.length() - 3);
  }
  jsonText.trim();

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, jsonText);
  if (error) {
    Serial.print("[JSON] Error: ");
    Serial.println(error.c_str());
    return false;
  }

  decision.respuesta = doc["respuesta"] | "Sin respuesta";
  decision.turnLed = doc["turn_led"] | false;
  return true;
}

String extractGeminiText(const String& payload) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    return "Error parsing API response: " + String(error.c_str());
  }
  
  if (doc["error"]) {
    return "API Error: " + String(doc["error"]["message"].as<const char*>());
  }

  if (doc["candidates"][0]["content"]["parts"][0]["text"].is<const char*>()) {
    return String(doc["candidates"][0]["content"]["parts"][0]["text"].as<const char*>());
  }
  return "Error: Unexpected API response structure";
}


void processJoke(const String& joke) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] No conectado");
    return;
  }

  Serial.print("[USER] Enviando chiste: ");
  Serial.println(joke);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url = "https://generativelanguage.googleapis.com/v1beta/models/" + 
               String(Config::kGeminiModel) + ":generateContent?key=" + String(Config::kGeminiToken);

  JsonDocument requestDoc;
  requestDoc["systemInstruction"]["parts"][0]["text"] = Config::kGeminiSystemInstruction;
  requestDoc["contents"][0]["parts"][0]["text"] = joke;
  requestDoc["generationConfig"]["responseMimeType"] = "application/json";
  requestDoc["generationConfig"]["temperature"] = Config::kGeminiTemperature;

  String requestBody;
  serializeJson(requestDoc, requestBody);

  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST(requestBody);
  if (httpCode > 0) {
    String payload = http.getString();
    String modelText = extractGeminiText(payload);
    
    GeminiDecision decision;
    if (parseGeminiDecision(modelText, decision)) {
      Serial.println("---------------------------");
      Serial.print("Gemini dice: ");
      Serial.println(decision.respuesta);
      Serial.print("Es gracioso? ");
      Serial.println(decision.turnLed ? "SI (LED ON)" : "NO (LED OFF)");
      Serial.println("---------------------------");

      digitalWrite(Config::kLedPin, decision.turnLed ? HIGH : LOW);
    } else {
      Serial.println("[ERROR] No se pudo procesar la decision de Gemini");
      Serial.println("Respuesta cruda del modelo: '" + modelText + "'");
      Serial.println("Payload completo de la API:");
      Serial.println(payload);
    }
  } else {
    Serial.print("[HTTP] Error: ");
    Serial.println(http.errorToString(httpCode));
  }
  http.end();
}

void setup() {
  Serial.begin(Config::kSerialBaudRate);
  pinMode(Config::kLedPin, OUTPUT);
  digitalWrite(Config::kLedPin, LOW);

  Serial.println("\n[SISTEMA] Iniciando...");
  
  WiFi.begin(Config::kWifiSsid, Config::kWifiPassword);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[WIFI] Conectado. IP: " + WiFi.localIP().toString());
  Serial.println("Envia un chiste por el Monitor Serial para que Gemini lo juzgue.");
}

void loop() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n') {
      serialBuffer.trim();
      if (serialBuffer.length() > 0) {
        processJoke(serialBuffer);
      }
      serialBuffer = "";
    } else if (c != '\r') {
      serialBuffer += c;
    }
  }
}
