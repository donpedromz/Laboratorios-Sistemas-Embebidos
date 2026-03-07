#include <Arduino.h>
/**
 * Semáforo para manejar el Mutex
 */
SemaphoreHandle_t xMutex = xSemaphoreCreateMutex();
void imprimir_mensaje_1(void *parameter){
  for(;;){
    xSemaphoreTake(xMutex, portMAX_DELAY);
    Serial.println("Tarea A: Escribiendo un mensaje muy extensoooo para probar el conflicto");
    xSemaphoreGive(xMutex);
    /**
     * Se encarga de cederle el procesador
     * a otra tarea, despúes de liberar el Mutex
     */
    taskYIELD();
  }
}
void imprimir_mensaje_2(void *parameter){
  for(;;){
    xSemaphoreTake(xMutex, portMAX_DELAY);
    Serial.println("Tarea B: Escribiendo otro mensaje muy extensoooooo");
    xSemaphoreGive(xMutex);
    /**
     * Se encarga de cederle el procesador
     * a otra tarea, despúes de liberar el Mutex
     */
    taskYIELD();
  }
}
void setup() {
  Serial.begin(9600);
  xTaskCreate(imprimir_mensaje_1,"Print 1",2048,NULL,1, NULL);
  xTaskCreate(imprimir_mensaje_2,"Print 2",2048,NULL,1, NULL);
}

void loop() {
}
