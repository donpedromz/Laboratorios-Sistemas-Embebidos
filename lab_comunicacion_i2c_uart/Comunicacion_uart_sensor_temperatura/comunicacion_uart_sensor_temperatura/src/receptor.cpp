#include <Arduino.h>
String message = "";
void setup()
{
  Serial.begin(9600);
}

void loop()
{
  while(Serial.available()){
    char c = Serial.read();
    message += c;
    if(c == '\n'){
      Serial.print("Recibido: ");
      Serial.print(message);
      message = "";
    }
  }
}