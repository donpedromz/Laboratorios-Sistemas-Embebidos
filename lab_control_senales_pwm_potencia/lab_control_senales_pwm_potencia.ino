int motor_output = 9;
int poten_input = A2;
void setup() {
  pinMode(motor_output, OUTPUT);
  pinMode(poten_input, INPUT);
  Serial.begin(9600);
}

void loop() {
  int valor_poten = analogRead(poten_input);
  int mapped_value = map(valor_poten, 0, 1023, 0, 255);
  analogWrite(motor_output, mapped_value);
  Serial.println(mapped_value);
}
