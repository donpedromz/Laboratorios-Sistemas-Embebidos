#include <Arduino.h>
#include <Wire.h> // Librería estándar para I2C
/*
Dado que tinkercard no soporta la libreria I2CScanner,
se usó una implementación manual usando Wire.h
*/
void setup() {
  Wire.begin();           // Inicia el bus I2C
  Serial.begin(9600);     // Inicia el puerto serie
  while (!Serial);        // Espera que el Serial esté listo 
  Serial.println("\nI2C Scanner iniciado");
}
void loop() {
  byte error, address;
  int nDevices = 0;

  Serial.println("Escaneando bus I2C...");
  for (address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    error = Wire.endTransmission();
    if (error == 0) {
      Serial.print("Dispositivo encontrado en dirección 0x");
      if (address < 16) Serial.print("0");
      Serial.println(address, HEX);
      nDevices++;
    } else if (error == 4) {
      Serial.print("Error desconocido en dirección 0x");
      if (address < 16) Serial.print("0");
      Serial.println(address, HEX);
    }
  }
  if (nDevices == 0)
    Serial.println("No se encontraron dispositivos I2C\n");
  else
    Serial.println("Escaneo completado\n");
  delay(5000); 
}