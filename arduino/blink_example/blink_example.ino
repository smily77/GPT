// Ein einfaches Arduino-Beispiel zum Blinken einer LED an Pin 13.

const int LED_PIN = 13;  // Der eingebaute LED-Pin auf vielen Arduino-Boards
const unsigned long BLINK_INTERVAL = 500;  // Intervall in Millisekunden

void setup() {
  pinMode(LED_PIN, OUTPUT);  // Setzt den LED-Pin als Ausgang
}

void loop() {
  digitalWrite(LED_PIN, HIGH);  // LED einschalten
  delay(BLINK_INTERVAL);        // 500 ms warten
  digitalWrite(LED_PIN, LOW);   // LED ausschalten
  delay(BLINK_INTERVAL);        // Nochmals 500 ms warten
}
