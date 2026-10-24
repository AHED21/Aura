#include <Arduino.h>
#include <DHT.h>

// --- DHT11 (temperature / humidite) ---
#define DHT_PIN   D2
#define DHT_TYPE  DHT11
DHT dht(DHT_PIN, DHT_TYPE);

// --- Capteur IR analogique (temperature infrarouge) ---
#define IR_PIN    A0

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Demarrage lecture capteurs...");

  dht.begin();
  analogReadResolution(12); // Nucleo L476RG: ADC 12 bits (0-4095)
}

void loop() {
  // --- Lecture DHT11 ---
  float humidity    = dht.readHumidity();
  float temperature = dht.readTemperature();

  if (isnan(humidity) || isnan(temperature)) {
    Serial.println("Erreur de lecture du DHT11 !");
  } else {
    Serial.print("DHT11  -> Temp: ");
    Serial.print(temperature);
    Serial.print(" C   Humidite: ");
    Serial.print(humidity);
    Serial.println(" %");
  }

  // --- Lecture capteur IR analogique ---
  int raw = analogRead(IR_PIN);
  float voltage = raw * (3.3 / 4095.0);
  Serial.print("IR     -> Brut: ");
  Serial.print(raw);
  Serial.print("   Tension: ");
  Serial.print(voltage, 3);
  Serial.println(" V");

  Serial.println("--------------------------------");
  delay(2000);
}
