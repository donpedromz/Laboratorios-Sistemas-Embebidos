#include <Arduino.h>
const int BUTTON_PIN = 16;
const int OUTPUT_LED = 17;
SemaphoreHandle_t xSemaforo = xSemaphoreCreateBinary();
void isr_toggle(){
  xSemaphoreGiveFromISR(xSemaforo, NULL);
}
void toggle_led(void* parameters){
  for(;;){
    xSemaphoreTake(xSemaforo, portMAX_DELAY);
    Serial.println("Encendiendo led.....");
    digitalWrite(OUTPUT_LED, HIGH);
    delay(2000);
    digitalWrite(OUTPUT_LED, LOW);
  }
}
void setup() {
  Serial.begin(9600);
  pinMode(BUTTON_PIN, INPUT);
  pinMode(OUTPUT_LED, OUTPUT);
  attachInterrupt(
    digitalPinToInterrupt(BUTTON_PIN), 
    isr_toggle, 
    CHANGE);

  xTaskCreate(toggle_led, "ToggleLED", 2048, NULL, 1, NULL);
}

void loop() {
  // put your main code here, to run repeatedly:
}