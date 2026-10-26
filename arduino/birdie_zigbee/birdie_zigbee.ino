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
 * ── Deux modes d'alimentation configurables ──────────────────────────────────
 * 1. Mode Batterie (PERMANENT_POWERED = false) :
 *    - Deep-Sleep basse consommation (12-18 mois d'autonomie)
 *    - Mesure SCD41 avant tout démarrage radio (élimine les chutes à 0)
 *    - Initialisation des clusters avec les vraies valeurs mesurées
 *    - Pas d'envoi en cas d'erreur de lecture I2C (conserve la valeur dans HA)
 * 
 * 2. Mode Secteur USB permanent (PERMANENT_POWERED = true) :
 *    - Zigbee connecté EN PERMANENCE (0 déconnexion, réactivité instantanée)
 *    - Mesures fluides et régulières dans loop() sans redémarrage à froid
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

// ── Configuration du mode d'alimentation ─────────────────────────────────────
// Mettez à 'true' si vous laissez le Birdie branché en USB sur secteur :
// Le Zigbee restera connecté en continu (0 déconnexion, réactivité maximale).
// Mettez à 'false' si vous utilisez une batterie LiPo (mode Deep-Sleep 5 min).
#define PERMANENT_POWERED       false

// Intervalles de sommeil en mode Batterie (Smart Eco Mode officiel) :
#define INTERVAL_NORMAL_SEC     300   // 5 minutes en air sain (< 1000 ppm)
#define INTERVAL_ALERT_SEC      60    // 1 minute pendant l'alerte (> 1000 ppm) pour compter les 10 min réelles
#define INTERVAL_DOWN_SEC       120   // 2 minutes quand l'oiseau est penché en bas

// Intervalle de mesure en mode Secteur USB permanent (secondes)
#define MAINS_INTERVAL_SEC      30    // 30 secondes

// Offset de calibration de température (en °C) si besoin d'ajustement
// (ex: -1.5f pour baisser de 1.5°C, 0.0f par défaut)
#define TEMP_OFFSET_DEG_C       0.0f

// ── Broches matérielles ──────────────────────────────────────────────────────
#define PIN_I2C_SDA             22
#define PIN_I2C_SCL             23
#define PIN_SERVO               2
#define PIN_ADC_BATTERY         0   // GPIO0 (A0)
#define PIN_BOOT_BUTTON         9   // Bouton BOOT sur XIAO ESP32-C6

// ── Endpoints Zigbee ────────────────────────────────────────────────────────
#define ED_CO2_ENDPOINT         10
#define ED_TEMP_ENDPOINT        11

// ── Angles et seuils physiques Birdie ────────────────────────────────────────
// Mettez à 'true' si dans votre montage le sens est inversé
#define SERVO_INVERT            false

#if !SERVO_INVERT
  #define SERVO_ANGLE_UP        180 // Poussé au maximum pour redresser l'oiseau
  #define SERVO_ANGLE_DOWN      0   // Poussé au maximum pour la descente
#else
  #define SERVO_ANGLE_UP        0   // Inversé : 0° en haut
  #define SERVO_ANGLE_DOWN      180 // Inversé : 180° en bas
#endif

#define THRESHOLD_CO2_HIGH      1000
#define THRESHOLD_CO2_LOW       950
#define MINUTES_TO_DROP         10

// ── Variables d'état persistantes en mémoire RTC ────────────────────────────
RTC_DATA_ATTR bool bird_is_down = false;
RTC_DATA_ATTR bool is_first_boot = true;
RTC_DATA_ATTR int  consecutive_minutes_above = 0;

SensirionI2CScd4x scd4x;
Servo airServo;

// Instanciation des capteurs Zigbee
ZigbeeCarbonDioxideSensor zbCO2Sensor = ZigbeeCarbonDioxideSensor(ED_CO2_ENDPOINT);
ZigbeeTempSensor zbTempSensor = ZigbeeTempSensor(ED_TEMP_ENDPOINT);

// ── Mouvement du servomoteur (Sécurisé avec 2.5s d'alimentation) ─────────────
void moveBird(bool down) {
  if (bird_is_down == down && !is_first_boot) return;

  Serial.printf("[SERVO] 🔄 Actionnement servomoteur -> %s (Angle cible: %d°)\n",
                down ? "BAS (Aérer !)" : "HAUT (Air sain)",
                down ? SERVO_ANGLE_DOWN : SERVO_ANGLE_UP);

  // Élargissement de la plage d'impulsion (500 à 2500 µs) pour aller chercher les 20° supplémentaires
  airServo.attach(PIN_SERVO, 500, 2500);
  airServo.write(down ? SERVO_ANGLE_DOWN : SERVO_ANGLE_UP);
  delay(2500); // 2.5 secondes complètes pour vaincre la friction et aller au bout
  airServo.detach(); // Coupe le PWM pour supprimer tout courant de veille
  bird_is_down = down;
}

// ── Test visuel de la course au tout premier démarrage ──────────────────────
void testServoSwing() {
  Serial.println("\n[SERVO TEST] 🧪 Test de calibration de course complète...");
  airServo.attach(PIN_SERVO, 500, 2500);

  Serial.printf("[SERVO TEST] 1. Vers position HAUT (%d°)...\n", SERVO_ANGLE_UP);
  airServo.write(SERVO_ANGLE_UP);
  delay(2000);

  Serial.printf("[SERVO TEST] 2. Vers position BAS (%d°)...\n", SERVO_ANGLE_DOWN);
  airServo.write(SERVO_ANGLE_DOWN);
  delay(2500);

  Serial.printf("[SERVO TEST] 3. Retour position HAUT (%d°)...\n", SERVO_ANGLE_UP);
  airServo.write(SERVO_ANGLE_UP);
  delay(2500);

  airServo.detach();
  bird_is_down = false;
  Serial.println("[SERVO TEST] ✅ Test terminé avec succès ! Oiseau prêt en HAUT.\n");
}

// ── Mesure de la tension batterie ────────────────────────────────────────────
float readBatteryVoltage() {
  uint32_t mv = analogReadMilliVolts(PIN_ADC_BATTERY);
  return (mv * 2.0f) / 1000.0f; // Pont diviseur 100k/100k (facteur 2)
}

int getBatteryPercentage(float v) {
  if (v >= 4.15f) return 100;
  if (v <= 3.25f) return 0;
  if (v > 3.80f)  return 60 + (int)((v - 3.80f) * (40.0f / (4.15f - 3.80f)));
  if (v > 3.65f)  return 20 + (int)((v - 3.65f) * (40.0f / (3.80f - 3.65f)));
  return (int)((v - 3.25f) * (20.0f / (3.65f - 3.25f)));
}

// ── Lecture robuste du capteur SCD41 (Anti-valeurs à 0) ──────────────────────
bool readSCD41(uint16_t &co2, float &temp, float &hum) {
#if PERMANENT_POWERED
  // En mode secteur permanent : lecture périodique en continu
  bool dataReady = false;
  uint16_t drErr = scd4x.getDataReadyFlag(dataReady);
  if (drErr != 0 || !dataReady) {
    return false;
  }
#else
  // En mode batterie : déclenchement Single-Shot
  uint16_t err = scd4x.measureSingleShot();
  if (err) {
    Serial.printf("[SCD41] ⚠️ Erreur measureSingleShot (%u), tentative périodique...\n", err);
    scd4x.startPeriodicMeasurement();
    delay(5000);
  } else {
    delay(5000); // 5000 ms nécessaires pour la conversion photo-acoustique NDIR
  }

  bool dataReady = false;
  for (int i = 0; i < 20; i++) {
    uint16_t drErr = scd4x.getDataReadyFlag(dataReady);
    if (drErr == 0 && dataReady) break;
    delay(100);
  }

  if (!dataReady) {
    Serial.println("[SCD41] ❌ Données non prêtes après timeout.");
    return false;
  }
#endif

  uint16_t rawCo2 = 0;
  float rawTemp = 0.0f;
  float rawHum = 0.0f;
  uint16_t readErr = scd4x.readMeasurement(rawCo2, rawTemp, rawHum);

  if (readErr != 0) {
    Serial.printf("[SCD41] ❌ Erreur lecture I2C/CRC (code %u)\n", readErr);
    return false;
  }

  // Contrôle de vraisemblance physique strict (élimine tout zéro accidentel)
  if (rawCo2 < 350 || rawCo2 > 10000 || rawTemp < -20.0f || rawTemp > 70.0f || rawHum < 0.0f || rawHum > 100.0f) {
    Serial.printf("[SCD41] ❌ Données physiques aberrantes ignorées (CO2: %u ppm, Temp: %.1f °C)\n", rawCo2, rawTemp);
    return false;
  }

  co2 = rawCo2;
  temp = rawTemp;
  hum = rawHum;
  return true;
}

// ── Envoi du rapport Zigbee ─────────────────────────────────────────────────
void sendDataReport(uint16_t co2, float temp, float hum, int batPct, float batVolts) {
  Serial.printf("[ZIGBEE REPORT] CO2: %u ppm | Temp: %.1f °C | Hum: %.1f %% | Bat: %d %% (%.2f V)\n",
                co2, temp, hum, batPct, batVolts);

  // Mise à jour et rapport CO2
  zbCO2Sensor.setCarbonDioxide((float)co2);
  zbCO2Sensor.report();

  // Mise à jour et rapport Température / Humidité / Batterie
  zbTempSensor.setTemperature(temp);
  zbTempSensor.setHumidity(hum);
  zbTempSensor.setBatteryPercentage(batPct);
  zbTempSensor.setBatteryVoltage((uint8_t)(batVolts * 10.0f)); // en 100mV (ex: 41 pour 4.1V)
  zbTempSensor.report();
  zbTempSensor.reportBatteryPercentage();
}

// ── Gestion du bouton BOOT (Appairage & Réinitialisation Usine) ──────────────
void checkBootButtonReset() {
  if (digitalRead(PIN_BOOT_BUTTON) == LOW) {
    delay(100);
    unsigned long pressStart = millis();
    while (digitalRead(PIN_BOOT_BUTTON) == LOW) {
      delay(50);
      if (millis() - pressStart > 3000) {
        Serial.println("\n[ZIGBEE] 🔄 Réinitialisation usine Zigbee demandée !");
        Zigbee.factoryReset();
        ESP.restart();
      }
    }
  }
}

// ── Logique officielle Birdie (10 min > 1000 ppm) ───────────────────────────
void updateBirdLogic(uint16_t co2, uint32_t &next_sleep_sec) {
  if (!bird_is_down) {
    if (co2 > THRESHOLD_CO2_HIGH) {
      consecutive_minutes_above++;
      Serial.printf("[ALERTE] CO2 > 1000 ppm (%d/%d min)\n", consecutive_minutes_above, MINUTES_TO_DROP);
      if (consecutive_minutes_above >= MINUTES_TO_DROP) {
        Serial.println("[ACTION] 🚨 10 minutes consécutives dépassées -> Descente de l'oiseau !");
        moveBird(true);
        next_sleep_sec = INTERVAL_DOWN_SEC; // 2 min quand l'oiseau est penché
      } else {
        // En mode comptage alerte : on dort 60 secondes pour compter 1 min à la fois
        next_sleep_sec = INTERVAL_ALERT_SEC;
      }
    } else {
      if (consecutive_minutes_above > 0) {
        Serial.println("[INFO] Air repassé sous 1000 ppm. Compteur réinitialisé.");
      }
      consecutive_minutes_above = 0;
      next_sleep_sec = INTERVAL_NORMAL_SEC; // 5 min en air sain
    }
  } else {
    if (co2 < THRESHOLD_CO2_LOW) {
      Serial.printf("[INFO] Air assaini (< %d ppm). Remontée de l'oiseau.\n", THRESHOLD_CO2_LOW);
      moveBird(false);
      consecutive_minutes_above = 0;
      next_sleep_sec = INTERVAL_NORMAL_SEC; // 5 min en air sain
    } else {
      Serial.printf("[INFO] Air toujours pollué (%u ppm). Maintien en bas.\n", co2);
      next_sleep_sec = INTERVAL_DOWN_SEC;   // 2 min de veille
    }
  }
}

// ── Initialisation Zigbee avec valeurs par défaut valides ────────────────────
void configureAndStartZigbee(uint16_t initialCo2, float initialTemp, float initialHum) {
  // 1. Capteur CO2
  zbCO2Sensor.setManufacturerAndModel("Birdie-DIY", "Birdie-CO2-Monitor");
  zbCO2Sensor.setMinMaxValue(0, 5000);
  zbCO2Sensor.setDefaultValue((float)initialCo2); // Évite tout rapport initial à 0 !
  Zigbee.addEndpoint(&zbCO2Sensor);

  // 2. Capteur Temp / Hum / Batterie
  zbTempSensor.setManufacturerAndModel("Birdie-DIY", "Birdie-CO2-Monitor");
  zbTempSensor.setMinMaxValue(-10, 60);
  zbTempSensor.setDefaultValue(initialTemp);      // Évite tout rapport initial à 0.0 °C !
  zbTempSensor.addHumiditySensor(0, 100, 1, initialHum);
  zbTempSensor.setPowerSource(ZB_POWER_SOURCE_BATTERY, 100, 37);
  Zigbee.addEndpoint(&zbTempSensor);

  // 3. Démarrage de la pile Zigbee
  Serial.println("[ZIGBEE] Démarrage de la pile Zigbee...");
  if (!Zigbee.begin()) {
    Serial.println("[ZIGBEE] Erreur d'initialisation, redémarrage...");
    delay(1000);
    ESP.restart();
  }

  Serial.println("[ZIGBEE] Connexion / Recherche du réseau...");
  unsigned long startJoin = millis();
  while (!Zigbee.connected() && (millis() - startJoin < 45000)) {
    Serial.print(".");
    delay(500);
    checkBootButtonReset();
  }
  Serial.println();

  if (Zigbee.connected()) {
    Serial.println("[ZIGBEE] ✅ Connecté avec succès au coordinateur Zigbee !");
  } else {
    Serial.println("[ZIGBEE] ⚠️ Délai de connexion dépassé.");
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// SETUP
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(50);

  Serial.println("\n==========================================");
  Serial.printf("🐦 Birdie DIY — Zigbee 3.0 (Mode: %s)\n",
                PERMANENT_POWERED ? "Secteur Permanent (Toujours Connecté)" : "Batterie (Deep-Sleep)");
  Serial.println("==========================================");

  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);

  // 1. Initialisation I2C et stabilisation du bus
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  delay(100); // Laisse le temps aux lignes I2C de se stabiliser au réveil
  scd4x.begin(Wire);

#if PERMANENT_POWERED
  // En mode secteur : lancer les mesures périodiques automatiques
  scd4x.startPeriodicMeasurement();
#else
  // En mode batterie : arrêter les mesures périodiques pour single-shot
  scd4x.stopPeriodicMeasurement();
  delay(500); // 500 ms obligatoires selon la datasheet Sensirion
#endif

  if (TEMP_OFFSET_DEG_C != 0.0f) {
    scd4x.setTemperatureOffset(TEMP_OFFSET_DEG_C);
  }

  // Au premier démarrage physique (ou après flash / reset) :
  // Balancement complet Haut -> Bas -> Haut pour valider la course et vaincre la friction
  if (is_first_boot) {
    is_first_boot = false;
    consecutive_minutes_above = 0;
    testServoSwing();
  }

  // 2. Mesure physique AVANT de lancer la radio
  // (Ainsi, on a des données garanties et on ne rapporte JAMAIS 0)
  uint16_t co2 = 0;
  float temp = 0.0f, hum = 0.0f;
  bool measurementOk = false;

#if !PERMANENT_POWERED
  // En mode batterie, effectuer la mesure immédiatement
  measurementOk = readSCD41(co2, temp, hum);

  if (!measurementOk) {
    Serial.println("[AVERTISSEMENT] ❌ Échec mesure SCD41. Pas d'envoi radio pour préserver l'historique.");
    // Sommeil court de 60s pour retenter sans polluer Home Assistant avec des 0
    Wire.end();
    esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
    esp_deep_sleep_start();
    return;
  }
#else
  // En mode secteur permanent : valeurs par défaut saines en attendant la 1ère mesure de loop()
  co2 = 450;
  temp = 20.0f;
  hum = 50.0f;
#endif

  // 3. Mesure de la batterie
  float batVolts = readBatteryVoltage();
  int batPercent = getBatteryPercentage(batVolts);

  // 4. Initialisation et connexion Zigbee
  configureAndStartZigbee(co2, temp, hum);

#if !PERMANENT_POWERED
  // ── Cycle Mode Batterie (Deep-Sleep) ───────────────────────────────────────
  if (Zigbee.connected()) {
    sendDataReport(co2, temp, hum, batPercent, batVolts);
    // Attendre 2 secondes que la trame Zigbee soit acquittée (APS ACK)
    delay(2000);
  }

  // Mise à jour de la mécanique de l'oiseau et durée du prochain sommeil
  uint32_t next_sleep_sec = INTERVAL_NORMAL_SEC;
  updateBirdLogic(co2, next_sleep_sec);

  // Extinction propre et mise en sommeil
  Serial.printf("[SLEEP] Mise en veille profonde pour %u secondes...\n", next_sleep_sec);
  Serial.flush();
  Wire.end();
  esp_sleep_enable_timer_wakeup((uint64_t)next_sleep_sec * 1000000ULL);
  esp_deep_sleep_start();
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// LOOP (Actif uniquement en mode Secteur Permanent)
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
#if PERMANENT_POWERED
  static unsigned long lastMeasureTime = 0;
  checkBootButtonReset();

  if (millis() - lastMeasureTime >= ((unsigned long)MAINS_INTERVAL_SEC * 1000UL)) {
    lastMeasureTime = millis();

    uint16_t co2 = 0;
    float temp = 0.0f, hum = 0.0f;

    if (readSCD41(co2, temp, hum)) {
      float batVolts = readBatteryVoltage();
      int batPercent = 100; // Toujours 100% sur secteur USB

      if (Zigbee.connected()) {
        sendDataReport(co2, temp, hum, batPercent, batVolts);
      }
      uint32_t dummy_sleep = 0;
      updateBirdLogic(co2, dummy_sleep);
    } else {
      Serial.println("[SCD41] En attente de nouvelles données prêtes...");
    }
  }

  delay(100);
#endif
}
