#include <Arduino.h>
const int POTEN_INPUT = 15;
int potValue = 0;
QueueHandle_t xCola = xQueueCreate(5, sizeof(int));
void print_potenciometer_value(void * paramter){
  int value_readed;
  for(;;){
    if(xQueueReceive(xCola, &value_readed, portMAX_DELAY) == pdTRUE){
      Serial.print("Valor leido: ");
      Serial.println(value_readed);
    }
  }
}
void read_potenciometer_value(void * parameter){
  for(;;){
    int valor_poten = analogRead(POTEN_INPUT);
    xQueueSend(xCola, &valor_poten, 12);
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}
void setup() {
  Serial.begin(9600);
  pinMode(POTEN_INPUT, INPUT);
  analogReadResolution(10);
  analogSetPinAttenuation(POTEN_INPUT, ADC_11db);
  xTaskCreate(
    print_potenciometer_value
    ,"Print Value from Potenciometer",
    2048,
    NULL,
    1,
    NULL
  );
  xTaskCreate(
    read_potenciometer_value,
    "Read",
    2048,
    NULL,
    1,
    NULL
  );
}
void loop() {
}