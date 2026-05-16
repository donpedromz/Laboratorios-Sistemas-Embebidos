#ifndef GLOBALS_H
#define GLOBALS_H

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <PubSubClient.h>
#include <WiFi.h>
#include "types.h"

extern QueueHandle_t sensorQueue;
extern QueueHandle_t systemMsgQueue;
extern WiFiClient wifiClient;
extern PubSubClient mqttClient;

#endif
