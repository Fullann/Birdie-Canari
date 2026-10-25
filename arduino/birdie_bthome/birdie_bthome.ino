/*
 * 🐦 Birdie DIY — Version Bluetooth Low Energy BTHome V2
 * 
 * Ce firmware combine l'autonomie record des versions autonomes avec l'intégration
 * instantanée dans Home Assistant sans configuration :
 * 
 * - 📡 Protocole BTHome V2 : diffusion par paquet publicitaire BLE ultra-court (~250 ms)
 * - 🏠 Auto-Discovery Home Assistant : reconnu instantanément par l'intégration BTHome
 *   (via Bluetooth natif ou ESP32 Bluetooth Proxy) sans aucun appairage ni code YAML !
 * - ⏱️ Autonomie estimée : 12 à 18 mois sur batterie LiPo 3800 mAh
 * - 🔊 Gazouillis d'oiseau (Buzzer sur D3 / GPIO21) : chant mélodieux lors de l'alerte CO2
 * - 🔋 Gestion de batterie (lecture GPIO0, alerte visuelle < 3.4V, coupure sécurité < 3.25V)
 * - 🎯 Calibration forcée FRC (400 ppm) via appui 5s sur le bouton BOOT (GPIO9)
 * - 💤 Deep-Sleep (< 10 µA) + Light-Sleep du CPU pendant la mesure SCD41
 * 
 * Câblage XIAO ESP32-C6 :
 * - SDA    : GPIO22 (D4)
 * - SCL    : GPIO23 (D5)
 * - SG90   : GPIO2  (D2)
 * - BUZZER : GPIO21 (D3 - fil + vers D3, fil - vers GND) [Optionnel]
 * - BAT+   : GPIO0  (A0 - Point milieu pont 100k/100k)
 * - BOOT   : GPIO9  (Bouton intégré sur la carte XIAO)
 * 
 * Configuration Arduino IDE requise :
 * - Carte : "Seeed Studio XIAO ESP32C6" (ou "ESP32C6 Dev Module")
 * - Bibliothèques requises :
 *     • "Sensirion I2C SCD4x" par Sensirion
 *     • "ESP32Servo" par Kevin Harrington
 */

#include <Arduino.h>
#include <Wire.h>
#include <SensirionI2CScd4x.h>
#include <ESP32Servo.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEAdvertising.h>
#include "esp_sleep.h"

// ── Broches matérielles ──────────────────────────────────────────────────────
#define PIN_I2C_SDA         22
#define PIN_I2C_SCL         23
#define PIN_SERVO           2
#define PIN_BUZZER          21  // Broche D3 (GPIO21 sur XIAO ESP32-C6) pour buzzer passif
#define PIN_ADC_BATTERY     0   // GPIO0 (A0 sur XIAO)
#define PIN_BOOT_BUTTON     9   // Bouton BOOT (Calibration FRC)

// ── Options configurables ───────────────────────────────────────────────────
#define ENABLE_BUZZER       true // Mettre à false pour couper le son
#define USE_LIGHT_SLEEP     1    // Sommeil léger CPU pendant conversion SCD41

// ── Angles du servomoteur SG90 ──────────────────────────────────────────────
#define SERVO_ANGLE_UP      180
#define SERVO_ANGLE_DOWN    0
#define SERVO_ANGLE_MID     90

// ── Seuils CO2 (ppm) ─────────────────────────────────────────────────────────
#define THRESHOLD_CO2_HIGH  1000 // Alerte haute
#define THRESHOLD_CO2_LOW   950  // Rétablissement
#define MINUTES_TO_DROP     10   // Minutes consécutives avant descente

// ── Seuils Batterie LiPo (Volts) ─────────────────────────────────────────────
#define BATTERY_LOW_VOLTS   3.40f // Alerte batterie faible (~15%)
#define BATTERY_CRIT_VOLTS  3.25f // Coupure de sécurité (~3%)

// ── Intervalles de sommeil (secondes) ─────────────────────────────────────────
#define INTERVAL_NORMAL_SEC 300  // Envoi BTHome toutes les 5 min en air sain
#define INTERVAL_ALERT_SEC  60   // Envoi toutes les 1 min pendant le comptage alerte
#define INTERVAL_DOWN_SEC   120  // Envoi toutes les 2 min quand l'oiseau est en bas

// ── Variables persistantes en mémoire RTC ────────────────────────────────────
RTC_DATA_ATTR bool bird_is_down = false;
RTC_DATA_ATTR bool is_first_boot = true;
RTC_DATA_ATTR int  consecutive_minutes_above = 0;
RTC_DATA_ATTR bool low_battery_warned = false;

SensirionI2CScd4x scd4x;
Servo airServo;

// ── Gazouillis d'oiseau mélodieux via le buzzer PWM ──────────────────────────
void playBirdChirp() {
#if ENABLE_BUZZER
  pinMode(PIN_BUZZER, OUTPUT);
  // Gazouillis ascendant doux (glissando rapide)
  for (int f = 2200; f <= 3600; f += 120) {
    tone(PIN_BUZZER, f, 12);
    delay(12);
  }
  delay(35);
  for (int f = 2600; f <= 3900; f += 130) {
    tone(PIN_BUZZER, f, 14);
    delay(14);
  }
  noTone(PIN_BUZZER);
#endif
}

// ── Mouvement physique du servomoteur ────────────────────────────────────────
void moveBird(bool down) {
  Serial.printf("[SERVO] Mouvement oiseau -> %s\n", down ? "BAS (Alerte)" : "HAUT (Air sain)");
  airServo.attach(PIN_SERVO);
  airServo.write(down ? SERVO_ANGLE_DOWN : SERVO_ANGLE_UP);
  delay(1500);
  airServo.detach();
  bird_is_down = down;
}

// ── Alerte visuelle batterie faible ──────────────────────────────────────────
void alertLowBatteryVisual() {
  Serial.println("[BATTERIE] 🪫 ALERTE : Batterie faible ! Mouvement d'avertissement...");
  airServo.attach(PIN_SERVO);
  airServo.write(SERVO_ANGLE_MID);
  delay(500);
  airServo.write(45);
  delay(400);
  airServo.write(SERVO_ANGLE_MID);
  delay(400);
  airServo.write(bird_is_down ? SERVO_ANGLE_DOWN : SERVO_ANGLE_UP);
  delay(1000);
  airServo.detach();
}

// ── Mesure et pourcentage batterie ───────────────────────────────────────────
float readBatteryVoltage() {
  uint32_t mv = analogReadMilliVolts(PIN_ADC_BATTERY);
  return (mv * 2.0f) / 1000.0f; // Facteur 2 du pont diviseur 100k/100k
}

int getBatteryPercentage(float v) {
  if (v >= 4.15f) return 100;
  if (v <= 3.25f) return 0;
  if (v > 3.80f)  return 60 + (int)((v - 3.80f) * (40.0f / (4.15f - 3.80f)));
  if (v > 3.65f)  return 20 + (int)((v - 3.65f) * (40.0f / (3.80f - 3.65f)));
  return (int)((v - 3.25f) * (20.0f / (3.65f - 3.25f)));
}

// ── Émission BTHome V2 via Bluetooth Low Energy ──────────────────────────────
void broadcastBTHome(uint16_t co2, float temp, float hum, int batPct, float batVolts) {
  Serial.println("[BTHOME] 📡 Diffusion du paquet BTHome V2...");

  BLEDevice::init("Birdie DIY");
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();

  BLEAdvertisementData advertData;
  advertData.setFlags(0x06); // General discoverable + BR_EDR_NOT_SUPPORTED

  // Construction du payload conforme à la norme BTHome V2
  uint8_t payload[20];
  int idx = 0;

  payload[idx++] = 0x40; // BTHome V2 (non-chiffré)

  // 1. Pourcentage batterie (ID 0x01, uint8)
  payload[idx++] = 0x01;
  payload[idx++] = (uint8_t)batPct;

  // 2. Température en °C (ID 0x02, int16 x 100)
  int16_t tempInt = (int16_t)(temp * 100.0f);
  payload[idx++] = 0x02;
  payload[idx++] = (uint8_t)(tempInt & 0xFF);
  payload[idx++] = (uint8_t)((tempInt >> 8) & 0xFF);

  // 3. Humidité relative en % (ID 0x03, uint16 x 100)
  uint16_t humInt = (uint16_t)(hum * 100.0f);
  payload[idx++] = 0x03;
  payload[idx++] = (uint8_t)(humInt & 0xFF);
  payload[idx++] = (uint8_t)((humInt >> 8) & 0xFF);

  // 4. CO2 en ppm (ID 0x12, uint16 x 1)
  payload[idx++] = 0x12;
  payload[idx++] = (uint8_t)(co2 & 0xFF);
  payload[idx++] = (uint8_t)((co2 >> 8) & 0xFF);

  // 5. Tension de batterie en mV (ID 0x0C, uint16 x 1)
  uint16_t voltMv = (uint16_t)(batVolts * 1000.0f);
  payload[idx++] = 0x0C;
  payload[idx++] = (uint8_t)(voltMv & 0xFF);
  payload[idx++] = (uint8_t)((voltMv >> 8) & 0xFF);

  String serviceData((char*)payload, idx);
  advertData.setServiceData(BLEUUID((uint16_t)0xFCD2), serviceData);
  advertData.setName("Birdie DIY");

  pAdvertising->setAdvertisementData(advertData);
  pAdvertising->start();

  // Diffusion pendant 250 ms (suffisant pour être capté par les proxys Bluetooth HA)
  delay(250);

  pAdvertising->stop();
  BLEDevice::deinit(true);
  Serial.println("[BTHOME] ✅ Paquet diffusé. Radio BLE éteinte.");
}

// ── Calibration forcée FRC (400 ppm) via appui long BOOT ─────────────────────
void checkAndHandleCalibration() {
  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
  if (digitalRead(PIN_BOOT_BUTTON) == LOW) {
    Serial.println("[CALIBRATION] Bouton BOOT détecté ! Maintenez 5s pour calibration FRC 400 ppm...");
    unsigned long start = millis();
    bool held = true;
    while (millis() - start < 5000) {
      if (digitalRead(PIN_BOOT_BUTTON) != LOW) {
        held = false;
        break;
      }
      delay(100);
    }

    if (held) {
      Serial.println("\n[CALIBRATION] 🎯 Lancement de la calibration forcée...");
      scd4x.stopPeriodicMeasurement();
      delay(500);

      uint16_t frcCorrection = 0;
      uint16_t err = scd4x.performForcedRecalibration(400, frcCorrection);

      if (err == 0 && frcCorrection != 0xFFFF) {
        Serial.printf("[CALIBRATION] ✅ Succès ! Correction: %d ppm\n", (int16_t)frcCorrection - 0x8000);
        scd4x.persistSettings();

        // Gazouillis de confirmation et battement de l'oiseau
        playBirdChirp();
        airServo.attach(PIN_SERVO);
        for (int i = 0; i < 2; i++) {
          airServo.write(180); delay(250);
          airServo.write(135); delay(250);
        }
        airServo.write(180); delay(300);
        airServo.detach();
      } else {
        Serial.println("[CALIBRATION] ❌ Échec de la calibration FRC.");
      }
    }
  }
}

// ── Mise en veille profonde ──────────────────────────────────────────────────
void enterDeepSleep(uint64_t seconds) {
  Wire.end();
  esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
  Serial.printf("[SLEEP] Deep-Sleep pendant %llu s (< 10 µA)...\n\n", seconds);
  Serial.flush();
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(30);

  Serial.println("==========================================");
  Serial.println("🐦 Birdie DIY — BTHome V2 (BLE + Buzzer)");
  Serial.println("==========================================");

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  scd4x.begin(Wire);

  // 1. Test calibration FRC
  checkAndHandleCalibration();

  // 2. Mesure batterie
  float batVolts = readBatteryVoltage();
  int batPercent = getBatteryPercentage(batVolts);
  Serial.printf("[BATTERIE] Tension: %.2f V | Niveau: %d %%\n", batVolts, batPercent);

  // Coupure sécurité batterie
  if (batVolts < BATTERY_CRIT_VOLTS && batVolts > 1.0f) {
    Serial.println("[BATTERIE] 🛑 NIVEAU CRITIQUE ! Mise en sommeil de sécurité.");
    moveBird(true);
    enterDeepSleep(3600);
    return;
  }

  // Alerte visuelle batterie faible
  if (batVolts < BATTERY_LOW_VOLTS && batVolts > 1.0f && !low_battery_warned) {
    alertLowBatteryVisual();
    low_battery_warned = true;
  } else if (batVolts >= BATTERY_LOW_VOLTS) {
    low_battery_warned = false;
  }

  // 3. Premier allumage physique
  if (is_first_boot) {
    is_first_boot = false;
    consecutive_minutes_above = 0;
    moveBird(false);
  }

  // 4. Mesure SCD41 Single-Shot
  scd4x.stopPeriodicMeasurement();
  uint16_t err = scd4x.measureSingleShot();
  if (err) {
    scd4x.startPeriodicMeasurement();
    delay(5000);
  } else {
#if USE_LIGHT_SLEEP
    esp_sleep_enable_timer_wakeup(5000 * 1000ULL);
    esp_light_sleep_start();
#else
    delay(5000);
#endif
  }

  uint16_t co2 = 0;
  float temp = 0.0f;
  float hum = 0.0f;
  bool dataReady = false;

  for (int i = 0; i < 10; i++) {
    scd4x.getDataReadyFlag(dataReady);
    if (dataReady) break;
    delay(100);
  }

  if (!dataReady) {
    Serial.println("[WARN] Données SCD41 non prêtes. Réessai dans 1 min.");
    enterDeepSleep(INTERVAL_ALERT_SEC);
    return;
  }

  scd4x.readMeasurement(co2, temp, hum);
  Serial.printf("[DATA] CO2: %u ppm | Temp: %.1f °C | Hum: %.1f %% | Oiseau: %s\n",
                co2, temp, hum, bird_is_down ? "BAS" : "HAUT");

  // 5. Diffusion des données vers Home Assistant (BTHome V2)
  broadcastBTHome(co2, temp, hum, batPercent, batVolts);

  // 6. Logique mécanique et gazouillis sonore
  uint64_t next_sleep_sec = INTERVAL_ALERT_SEC;

  if (!bird_is_down) {
    if (co2 > THRESHOLD_CO2_HIGH) {
      consecutive_minutes_above++;
      Serial.printf("[ALERTE] CO2 > 1000 ppm depuis %d min (Seuil: %d min)\n",
                    consecutive_minutes_above, MINUTES_TO_DROP);

      if (consecutive_minutes_above >= MINUTES_TO_DROP) {
        Serial.println("[ACTION] 10 min consécutives dépassées -> Descente de l'oiseau + Gazouillis !");
        playBirdChirp();  // 🎶 Gazouillis d'alerte discret
        moveBird(true);
        next_sleep_sec = INTERVAL_DOWN_SEC;
      } else {
        next_sleep_sec = INTERVAL_ALERT_SEC;
      }
    } else {
      consecutive_minutes_above = 0;
      next_sleep_sec = INTERVAL_NORMAL_SEC; // 5 min en air sain
    }
  } else {
    if (co2 < THRESHOLD_CO2_LOW) {
      Serial.printf("[ACTION] CO2 < %d ppm (%u ppm) -> Remontée de l'oiseau !\n",
                    THRESHOLD_CO2_LOW, co2);
      moveBird(false);
      consecutive_minutes_above = 0;
      next_sleep_sec = INTERVAL_NORMAL_SEC;
    } else {
      Serial.println("[INFO] Air toujours pollué. Maintien en bas.");
      next_sleep_sec = INTERVAL_DOWN_SEC;
    }
  }

  enterDeepSleep(next_sleep_sec);
}

void loop() {
  // Vide
}
