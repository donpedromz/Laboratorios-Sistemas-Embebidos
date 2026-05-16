#ifndef NETWORK_H
#define NETWORK_H

#include <Arduino.h>

void networkInit();
bool wifiConnect();
bool mqttConnect();
void mqttLoop();

#endif
