// Pin de salida PWM para el motor
const int motor_output = 9;
// Pin de entrada analoga proveniente del potenciometro
const int poten_input = A2;
void setup() {
  // Se establece el pin del motor como salida
  pinMode(motor_output, OUTPUT);
  // Se establece el pin del potenciometro como entrada
  pinMode(poten_input, INPUT);
  // Se inicia la comunicacion Serial para imprimir en terminal el valor mapeado
  Serial.begin(9600);
}

void loop() {
  // Lectura del valor del potenciometro
  int valor_poten = analogRead(poten_input);
  /**
   * Se convierte el valor de lectura del potenciometro a un valor PWM
   * Potenciometro -> rango [0,1023]
   * PWM -> rango [0,255]
   */
  int mapped_value = map(valor_poten, 0, 1023, 0, 255);
  // Se envia el valor PWM al transistor conectado en la protoboard
  analogWrite(motor_output, mapped_value);
  // Se imprime el valor PWM obtenido despues del map.
  Serial.println(mapped_value);
}
