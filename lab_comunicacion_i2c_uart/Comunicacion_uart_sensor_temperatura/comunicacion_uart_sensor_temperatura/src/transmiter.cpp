#include<Arduino.h>
const int TEMPERATURE_LECT = A0;
void setup()
{
  pinMode(TEMPERATURE_LECT, INPUT);
  Serial.begin(9600);
}

void loop()
{
  int reading = analogRead(TEMPERATURE_LECT); 
  float voltage = reading * 5.0;
  voltage /= 1024.0; 
  /**
   * Convertir la lectura del voltage a
   * una temperatura en °C
   */
  float temperatureC = (voltage - 0.5) * 100 ;  
  Serial.print("Temperatura detectada: ");
  Serial.print(temperatureC);
  Serial.println("C");
  delay(1000);
}