/*
 * 🐦 Birdie DIY — Firmware Zigbee 3.0 Natif pour Seeed Studio XIAO ESP32-C6
 * 
 * Basé sur la bibliothèque officielle Arduino Zigbee d'Espressif (Core 3.x) :
 * - Compatible nativement avec Zigbee2MQTT et Home Assistant (ZHA)
 * - Capteur de CO2 (ppm) via ZigbeeCarbonDioxideSensor
 * - Capteur Température (°C) et Humidité (%) via ZigbeeTempSensor
 * - Niveau de batterie (%)
 * - Règle officielle Birdie : descente après 10 min > 1000 ppm, remontée < 950 ppm
 * 
 * Appairage (Pairing) :
 * - Au premier allumage, le Birdie entre automatiquement en mode appairage.
 * - Maintien du bouton BOOT (GPIO9) pendant 3 secondes = Réinitialisation usine & nouvel appairage.
 */

#include <Arduino.h>
#ifndef ZIGBEE_MODE_ED
#define ZIGBEE_MODE_ED
#endif

#include <Wire.h>
#include <SensirionI2CScd4x.h>
#include <ESP32Servo.h>
#include "Zigbee.h"

// ── Broches matérielles ──────────────────────────────────────────────────────
#define PIN_I2C_SDA         22
#define PIN_I2C_SCL         23
#define PIN_SERVO           2
#define PIN_ADC_BATTERY     0   // GPIO0 (A0)
#define PIN_BOOT_BUTTON     9   // Bouton BOOT sur XIAO ESP32-C6

// ── Endpoints Zigbee ────────────────────────────────────────────────────────
#define ED_CO2_ENDPOINT     10
#define ED_TEMP_ENDPOINT    11

// ── Angles et seuils physiques Birdie ────────────────────────────────────────
// Marge de 10° pour éviter que le servo ne force contre les butées du boîtier
#define SERVO_ANGLE_UP      170 // Oiseau dressé en haut (air sain)
#define SERVO_ANGLE_DOWN    10  // Oiseau penché en bas (aérer !)
#define THRESHOLD_CO2_HIGH  1000
#define THRESHOLD_CO2_LOW   950
#define MINUTES_TO_DROP     10

// Intervalle de rapport périodique (en secondes)
#define REPORT_INTERVAL_SEC 300 // 5 minutes

// ── Variables d'état persistantes en mémoire RTC ────────────────────────────
RTC_DATA_ATTR bool bird_is_down = false;
RTC_DATA_ATTR bool is_first_boot = true;
RTC_DATA_ATTR int  consecutive_minutes_above = 0;

SensirionI2CScd4x scd4x;
Servo airServo;

// Instanciation des capteurs Zigbee
ZigbeeCarbonDioxideSensor zbCO2Sensor = ZigbeeCarbonDioxideSensor(ED_CO2_ENDPOINT);
ZigbeeTempSensor zbTempSensor = ZigbeeTempSensor(ED_TEMP_ENDPOINT);

// ── Mouvement du servomoteur (Sécurisé) ──────────────────────────────────────
void moveBird(bool down) {
  // Sécurité : si l'oiseau est déjà dans la position demandée, ne rien faire
  if (bird_is_down == down && !is_first_boot) return;

  Serial.printf("[SERVO] Position oiseau -> %s\n", down ? "BAS (Aérer !)" : "HAUT (Air sain)");
  airServo.setPeriodHertz(50);
  airServo.attach(PIN_SERVO, 500, 2400);
  airServo.write(down ? SERVO_ANGLE_DOWN : SERVO_ANGLE_UP);
  delay(800); // 800 ms suffisent largement pour tourner sans chauffer
  airServo.detach(); // Coupe immédiatement l'alimentation du moteur
  bird_is_down = down;
}


// ── Mesure de la tension batterie ────────────────────────────────────────────
float readBatteryVoltage() {
  uint32_t mv = analogReadMilliVolts(PIN_ADC_BATTERY);
  return (mv * 2.0f) / 1000.0f; // Pont diviseur R1=R2 (facteur 2)
}

int getBatteryPercentage(float v) {
  if (v >= 4.15f) return 100;
  if (v <= 3.25f) return 0;
  if (v > 3.80f)  return 60 + (int)((v - 3.80f) * (40.0f / (4.15f - 3.80f)));
  if (v > 3.65f)  return 20 + (int)((v - 3.65f) * (40.0f / (3.80f - 3.65f)));
  return (int)((v - 3.25f) * (20.0f / (3.65f - 3.25f)));
}

// ── Lecture du capteur SCD41 ────────────────────────────────────────────────
bool readSCD41(uint16_t &co2, float &temp, float &hum) {
  uint16_t err = scd4x.measureSingleShot();
  if (err) {
    scd4x.startPeriodicMeasurement();
    delay(5000);
  } else {
    delay(5000); // Temps de mesure optique NDIR du SCD41
  }

  bool dataReady = false;
  for (int i = 0; i < 15; i++) {
    scd4x.getDataReadyFlag(dataReady);
    if (dataReady) break;
    delay(100);
  }

  if (dataReady) {
    scd4x.readMeasurement(co2, temp, hum);
    return true;
  }
  return false;
}

// ── Envoi du rapport Zigbee ─────────────────────────────────────────────────
void sendDataReport(uint16_t co2, float temp, float hum, int batPct, float batVolts) {
  Serial.printf("[ZIGBEE REPORT] CO2: %u ppm | Temp: %.1f °C | Hum: %.1f %% | Bat: %d %% (%.2f V)\n",
                co2, temp, hum, batPct, batVolts);

  // Mise à jour et rapport CO2
  zbCO2Sensor.setCarbonDioxide((float)co2);
  zbCO2Sensor.report();

  // Mise à jour et rapport Température / Humidité
  zbTempSensor.setTemperature(temp);
  zbTempSensor.setHumidity(hum);
  zbTempSensor.setBatteryPercentage(batPct);
  zbTempSensor.setBatteryVoltage((uint32_t)(batVolts * 10)); // en 0.1V
  zbTempSensor.report();
}

void setup() {
  Serial.begin(115200);
  delay(50);

  Serial.println("\n==========================================");
  Serial.println("🐦 Birdie DIY — Zigbee 3.0 (ESP32-C6)");
  Serial.println("==========================================");

  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);

  // Configuration I2C
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  scd4x.begin(Wire);
  scd4x.stopPeriodicMeasurement();

  // Position initiale de l'oiseau au premier allumage
  if (is_first_boot) {
    is_first_boot = false;
    consecutive_minutes_above = 0;
    moveBird(false);
  }

  // 1. Configuration du capteur CO2 Zigbee
  zbCO2Sensor.setManufacturerAndModel("Birdie-DIY", "Birdie-CO2-Monitor");
  zbCO2Sensor.setMinMaxValue(0, 5000);
  Zigbee.addEndpoint(&zbCO2Sensor);

  // 2. Configuration du capteur Temp/Hum/Batterie Zigbee
  zbTempSensor.setManufacturerAndModel("Birdie-DIY", "Birdie-CO2-Monitor");
  zbTempSensor.setMinMaxValue(-10, 60);
  zbTempSensor.addHumiditySensor(0, 100, 1, 50.0);
  zbTempSensor.setPowerSource(ZB_POWER_SOURCE_BATTERY, 100, 37);
  Zigbee.addEndpoint(&zbTempSensor);

  // 3. Démarrage de la pile Zigbee
  Serial.println("[ZIGBEE] Démarrage de la pile Zigbee...");
  if (!Zigbee.begin()) {
    Serial.println("[ZIGBEE] Erreur d'initialisation, redémarrage...");
    delay(1000);
    ESP.restart();
  }

  Serial.println("[ZIGBEE] Recherche du réseau Zigbee (Mode Appairage)...");
  Serial.println("[ASTUCE] Activez 'Autoriser l'appairage' dans Zigbee2MQTT ou ZHA !");

  // Attente de la connexion Zigbee (jusqu'à 90 secondes au premier appairage)
  unsigned long startJoin = millis();
  while (!Zigbee.connected() && (millis() - startJoin < 90000)) {
    Serial.print(".");
    delay(500);

    // Appui long bouton BOOT (>3s) pour forcer une réinitialisation usine
    if (digitalRead(PIN_BOOT_BUTTON) == LOW) {
      delay(100);
      int pressTime = millis();
      while (digitalRead(PIN_BOOT_BUTTON) == LOW) {
        delay(50);
        if (millis() - pressTime > 3000) {
          Serial.println("\n[ZIGBEE] 🔄 Réinitialisation usine Zigbee demandée !");
          Zigbee.factoryReset();
          ESP.restart();
        }
      }
    }
  }

  Serial.println();
  if (Zigbee.connected()) {
    Serial.println("[ZIGBEE] ✅ Connecté avec succès au coordinateur Zigbee !");
  } else {
    Serial.println("[ZIGBEE] ⚠️ Délai d'appairage dépassé. Le Birdie continuera à fonctionner en local.");
  }

  // 4. Mesure SCD41
  uint16_t co2 = 0;
  float temp = 0.0f, hum = 0.0f;
  if (readSCD41(co2, temp, hum)) {
    Serial.printf("[MESURE] CO2: %u ppm | Temp: %.1f °C | Hum: %.1f %%\n", co2, temp, hum);
  } else {
    Serial.println("[ERREUR] Échec de lecture du SCD41 !");
    co2 = 400; // Valeur par défaut
  }

  // 5. Mesure Batterie
  float batVolts = readBatteryVoltage();
  int batPercent = getBatteryPercentage(batVolts);

  // 6. Envoi des données si connecté
  if (Zigbee.connected()) {
    sendDataReport(co2, temp, hum, batPercent, batVolts);
    delay(1000); // Laisser le temps à la radio de finaliser la transmission
  }

  // 7. Logique mécanique de l'oiseau (règle des 10 min consécutives)
  if (!bird_is_down) {
    if (co2 > THRESHOLD_CO2_HIGH) {
      consecutive_minutes_above++;
      Serial.printf("[ALERTE] CO2 > 1000 ppm (%d/%d min)\n", consecutive_minutes_above, MINUTES_TO_DROP);
      if (consecutive_minutes_above >= MINUTES_TO_DROP) {
        moveBird(true);
      }
    } else {
      consecutive_minutes_above = 0;
    }
  } else {
    if (co2 < THRESHOLD_CO2_LOW) {
      Serial.println("[INFO] Air sain (< 950 ppm). Remontée de l'oiseau.");
      moveBird(false);
      consecutive_minutes_above = 0;
    }
  }

  // 8. Mise en sommeil Deep-Sleep
  Serial.printf("[SLEEP] Mise en veille profonde pour %d secondes...\n", REPORT_INTERVAL_SEC);
  Serial.flush();
  Wire.end();
  esp_sleep_enable_timer_wakeup((uint64_t)REPORT_INTERVAL_SEC * 1000000ULL);
  esp_deep_sleep_start();
}

void loop() {
  // Cycle exécuté dans setup() au réveil
}
