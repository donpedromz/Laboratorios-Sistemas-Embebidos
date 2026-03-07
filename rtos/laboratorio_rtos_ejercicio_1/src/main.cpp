#include <Arduino.h>
void water_mark(void * parameters){
  for(;;){
    Serial.println(uxTaskGetStackHighWaterMark(NULL));
  }
}
void setup() {
  Serial.begin(9600);
  xTaskCreate(
    water_mark,
    "Print WaterMark",
    /**
     * Se me reinició el ESP32 en 
     * 256
     */
    256,
    NULL,
    1,
    NULL
  );
}

void loop() {
}
