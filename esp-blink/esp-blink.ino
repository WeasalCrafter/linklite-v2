void setup() {
	pinMode(23, OUTPUT);
}

void loop() {
	digitalWrite(23, HIGH);
	delay(1000);
	digitalWrite(23, LOW);
	delay(1000);
}
