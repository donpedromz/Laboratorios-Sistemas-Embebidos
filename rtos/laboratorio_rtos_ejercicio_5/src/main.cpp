#include <Arduino.h>
const int OUTPUT_LED = 18;
void prender_led(void * parameters){
  for(;;){
    Serial.printf("[LED][Nucleo %d] LED ENCENDIDO\r\n", xPortGetCoreID());
    digitalWrite(OUTPUT_LED, HIGH);
    delay(1500);
    Serial.printf("[LED][Nucleo %d] LED APAGADO\r\n", xPortGetCoreID());
    digitalWrite(OUTPUT_LED, LOW);
    delay(1500);
  }
}
void calculo_pesado(void *parameters){
  int i = 0;
  for(;;){
    i++;
    Serial.printf("[CALCULO][Nucleo %d] Valor Obtenido: %d\r\n", xPortGetCoreID(), i);
    /*
    vTaskDelay(1) cede la CPU al scheduler por 1 tick (~1ms).
    taskYIELD no funcionaba porque es la unica tarea en el nucleo 0,
    asi que se reanudaba inmediatamente e inundaba el Serial.
    */
    vTaskDelay(1);
  }
}
void setup() {
  Serial.begin(9600);
  pinMode(OUTPUT_LED, OUTPUT);
  xTaskCreatePinnedToCore(
    prender_led,
    "Prender Led",
    2048,
    NULL,
    1,
    NULL,
    1
  );
  xTaskCreatePinnedToCore(
    calculo_pesado,
    "Calculo Pesado",
    2048,
    NULL,
    1,
    NULL,
    0
  );
}

void loop() {
  // put your main code here, to run repeatedly:
}