#include <SPI.h>
#include <SD.h>

// ---- Thermistor ----
const int   ADC_PIN = 3;          // NTC divider junction
const float VSUPPLY = 3.30;
const float RFIX    = 15000.0;    // 15 kohm fixed resistor
const float R0      = 10000.0;    // NTC = 10k @ 25 C
const float T0      = 298.15;
const float BETA    = 3950.0;

// ---- SD card SPI pins ----
const int SD_SCK  = 4;
const int SD_MISO = 5;
const int SD_MOSI = 6;
const int SD_CS   = 7;

// ---- Status LED ----
const int LED_PIN = 8;            // flashes on each recording

const unsigned long PERIOD_MS = 500;   // record every 0.5 s
unsigned long nextTime = 0;

File logFile;

void setup() {
  Serial.begin(115200);
  delay(300);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS)) {
    Serial.println("SD init failed - check wiring/card format");
    while (true) delay(1000);
  }

  char fname[20];
  int n = 0;
  do { sprintf(fname, "/data_%03d.csv", n); n++; } while (SD.exists(fname));
  Serial.printf("Logging to %s\n", fname);

  logFile = SD.open(fname, FILE_WRITE);
  if (!logFile) { Serial.println("File open failed"); while (true) delay(1000); }
  logFile.println("time_s,temp_C");
  logFile.flush();
}

void loop() {
  if (millis() < nextTime) return;
  nextTime += PERIOD_MS;

  digitalWrite(LED_PIN, HIGH);            // flash ON while recording

  const int N = 32;
  uint32_t mv = 0;
  for (int i = 0; i < N; i++) { mv += analogReadMilliVolts(ADC_PIN); delay(2); }
  float vout = (mv / (float)N) / 1000.0;

  float rntc = RFIX * vout / (VSUPPLY - vout);
  float tK   = 1.0 / (1.0/T0 + (1.0/BETA) * log(rntc / R0));
  float tC   = tK - 273.15;

  float t_s = millis() / 1000.0;
  logFile.printf("%.2f,%.2f\n", t_s, tC);
  logFile.flush();                        // last line survives a power cut

  Serial.printf("%.2f s, %.2f C\n", t_s, tC);

  digitalWrite(LED_PIN, LOW);             // flash OFF
}