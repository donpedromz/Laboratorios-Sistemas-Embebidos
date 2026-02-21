#include <Arduino.h>
#include <Wire.h>
#include <MFRC522.h>
#include <LiquidCrystal_I2C.h>

#define RST_PIN 15
#define SS_PIN 5
#define SDA_PIN 21
#define SCL_PIN 22

/**
 * Lector RFID
 */
MFRC522 rfid(SS_PIN, RST_PIN);
/**
 * Pantalla LCD
 */
LiquidCrystal_I2C* lcd;
uint8_t lcdAddress = 0;
/**
 * Escanear la dirección de la pantalla LCD
 * mediante I2C
 */
uint8_t scanI2C() {
  Serial.println("[I2C] Iniciando escaneo...");
  for (uint8_t address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    uint8_t error = Wire.endTransmission();
    if (error == 0) {
      Serial.print("[I2C] Dispositivo encontrado en 0x");
      Serial.println(address, HEX);
      return address;
    }
    else if (error == 4) {
      Serial.print("[I2C] Error desconocido en 0x");
      Serial.println(address, HEX);
    }
  }
  Serial.println("[I2C] Escaneo finalizado. No se encontraron dispositivos.");
  return 0;
}

void setup() {
  Serial.begin(9600);
  delay(1000);
  Serial.println("=== INICIO DEL SISTEMA ===");
  Serial.println("[I2C] Inicializando bus...");
  Wire.begin(SDA_PIN, SCL_PIN);
  lcdAddress = scanI2C();
  if (lcdAddress == 0) {
    Serial.println("[ERROR] LCD NO DETECTADO");
    Serial.println("[SISTEMA BLOQUEADO] Verificar conexiones SDA/SCL");
    while (true) {
      delay(1000);
      Serial.println("[DEBUG] Esperando correccion de hardware...");
    }
  }

  Serial.print("[I2C] LCD detectado en 0x");
  Serial.println(lcdAddress, HEX);
  lcd = new LiquidCrystal_I2C(lcdAddress, 16, 2);
  Serial.println("[LCD] Inicializando display...");
  lcd->init();
  lcd->backlight();
  lcd->print("Sistema listo");
  Serial.println("[RFID] Inicializando lector...");
	SPI.begin();			
	rfid.PCD_Init();		
	delay(4);			
	rfid.PCD_DumpVersionToSerial();	
  byte v = rfid.PCD_ReadRegister(rfid.VersionReg);
  if (v == 0x00 || v == 0xFF) {
    Serial.println("[ERROR] RFID NO RESPONDE");
    Serial.println("[SISTEMA BLOQUEADO] Verificar conexion SPI");
    while (true) {
      delay(1000);
      Serial.println("[DEBUG] Esperando correccion de hardware...");
    }
  }

  Serial.print("[RFID] Version detectada: 0x");
  Serial.println(v, HEX);

  delay(2000);
  lcd->clear();
  lcd->print("Esperando RFID");

  Serial.println("=== SISTEMA LISTO ===");
}

void loop() {
  if (!rfid.PICC_IsNewCardPresent()) {
    return;
  }
  Serial.println("[RFID] Tarjeta detectada");

  if (!rfid.PICC_ReadCardSerial()) {
    Serial.println("[ERROR] No se pudo leer UID");
    return;
  }
  if (rfid.uid.size == 0) {
    Serial.println("[ERROR] UID invalido");
    return;
  }
  Serial.println("[RFID] UID leido correctamente");
  rfid.PICC_DumpToSerial(&(rfid.uid));
  lcd->clear();
  lcd->print("UID:");
  lcd->setCursor(0, 1);
  for (byte i = 0; i < rfid.uid.size; i++) {
    lcd->print(rfid.uid.uidByte[i], HEX);
    lcd->print(" ");
  }
  delay(2000);
  lcd->clear();
  lcd->print("Esperando RFID");
  rfid.PICC_HaltA();
  Serial.println("[RFID] Lectura finalizada");
}