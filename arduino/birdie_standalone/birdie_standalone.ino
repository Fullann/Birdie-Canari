/*
 * 🐦 Birdie DIY — Version Arduino C++ Ultra-Basse Consommation (100% Autonome)
 * 
 * Aucune connectivité (Sans Wi-Fi, Sans Bluetooth, Sans Cloud)
 * 
 * Fonctionnalités intégrées :
 * - Fonctionnement identique au Birdie® officiel (Cycle v1) :
 *     • Veille normale (< 1 000 ppm) : Réveil toutes les 10 minutes (600 s)
 *     • Alerte (> 1 000 ppm)         : Oiseau descendu, réveil toutes les 2 minutes (120 s)
 *     • Cooldown (< 950 ppm)         : Oiseau remonte, sommeil profond de 2 heures (7 200 s)
 * - 🔋 Gestion de batterie intégrée sur GPIO0 (A0)
 * - 🪫 Alerte visuelle via le servomoteur si batterie < 3.4V (15%)
 * - 🛑 Sommeil de sécurité si batterie critique < 3.25V
 * - Consommation en sommeil : < 10 µA (Deep-Sleep ESP32-C6)
 * - Réveil ultra-rapide : ~5 secondes pour la mesure SCD41 en mode single-shot
 * - Actionnement servo : Uniquement lors d'un changement d'état physique (détaché après 1.5s)
 * 
 * Câblage XIAO ESP32-C6 :
 * - SDA  : GPIO22 (D4)
 * - SCL  : GPIO23 (D5)
 * - SG90 : GPIO2  (D2)
 * - BAT+ : GPIO0  (A0 - Point milieu pont 100k/100k)
 * 
 * Bibliothèques Arduino requises :
 * - "Sensirion I2C SCD4x" par Sensirion
 * - "ESP32Servo" par Kevin Harrington
 */

#include <Arduino.h>
#include <Wire.h>
#include <SensirionI2CScd4x.h>
#include <ESP32Servo.h>
#include "esp_sleep.h"

// ── Broches ──────────────────────────────────────────────────────────────────
#define PIN_I2C_SDA      22
#define PIN_I2C_SCL      23
#define PIN_SERVO        2
#define PIN_ADC_BATTERY  0  // GPIO0 (A0 sur XIAO)

// ── Angles du servomoteur SG90 ──────────────────────────────────────────────
#define SERVO_ANGLE_UP    180  // Oiseau dressé en haut (air bon)
#define SERVO_ANGLE_DOWN  0    // Oiseau penché en bas (aérer !)
#define SERVO_ANGLE_MID   90   // Position intermédiaire pour alerte batterie

// ── Seuils Batterie LiPo (Volts) ─────────────────────────────────────────────
#define BATTERY_LOW_VOLTS   3.40f // Seuil alerte batterie faible (~15%)
#define BATTERY_CRIT_VOLTS  3.25f // Seuil critique : sommeil sécurité (~3%)

// ── Variables persistantes en mémoire RTC (survivent au Deep-Sleep) ──────────
RTC_DATA_ATTR bool bird_is_down = false;
RTC_DATA_ATTR bool is_first_boot = true;
RTC_DATA_ATTR bool low_battery_warned = false;

SensirionI2CScd4x scd4x;
Servo airServo;

void moveBird(bool down) {
  airServo.attach(PIN_SERVO, 500, 2500);
  airServo.write(down ? SERVO_ANGLE_DOWN : SERVO_ANGLE_UP);
  delay(2500); // Laisse 2.5s au moteur pour terminer sa rotation complète sous charge
  airServo.detach(); // Coupe le signal PWM pour stopper tout courant de maintien
  bird_is_down = down;
}

void alertLowBatteryVisual() {
  Serial.println("[BATTERIE] 🪫 Alerte : Batterie faible ! Mouvement d'avertissement...");
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

float readBatteryVoltage() {
  uint32_t mv = analogReadMilliVolts(PIN_ADC_BATTERY);
  return (mv * 2.0f) / 1000.0f; // Pont diviseur R1=R2 (facteur 2.0)
}

int getBatteryPercentage(float v) {
  if (v >= 4.15f) return 100;
  if (v <= 3.25f) return 0;
  if (v > 3.80f)  return 60 + (int)((v - 3.80f) * (40.0f / (4.15f - 3.80f)));
  if (v > 3.65f)  return 20 + (int)((v - 3.65f) * (40.0f / (3.80f - 3.65f)));
  return (int)((v - 3.25f) * (20.0f / (3.65f - 3.25f)));
}

void enterDeepSleep(uint64_t seconds) {
  Wire.end();
  esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
  Serial.printf("Mise en veille profonde pour %llu s (< 10 µA)...\n", seconds);
  Serial.flush();
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(50);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  scd4x.begin(Wire);
  scd4x.stopPeriodicMeasurement();

  // 1. Lecture de la tension et gestion de la batterie
  float batVolts = readBatteryVoltage();
  int batPercent = getBatteryPercentage(batVolts);
  Serial.printf("[BATTERIE] Tension: %.2f V | Niveau: %d %%\n", batVolts, batPercent);

  // Coupure sécurité si batterie critique
  if (batVolts < BATTERY_CRIT_VOLTS && batVolts > 1.0f) {
    Serial.println("[BATTERIE] 🛑 Batterie critique ! Mise en sommeil prolongé 1h.");
    moveBird(true);
    enterDeepSleep(3600);
    return;
  }

  // Alerte visuelle batterie faible (< 3.4V)
  if (batVolts < BATTERY_LOW_VOLTS && batVolts > 1.0f && !low_battery_warned) {
    alertLowBatteryVisual();
    low_battery_warned = true;
  } else if (batVolts >= BATTERY_LOW_VOLTS) {
    low_battery_warned = false;
  }

  // 2. Premier démarrage matériel
  if (is_first_boot) {
    is_first_boot = false;
    moveBird(false);
  }

  // 3. Mesure SCD41 Single-Shot
  uint16_t error = scd4x.measureSingleShot();
  if (error) {
    scd4x.startPeriodicMeasurement();
    delay(5000);
  } else {
    // Light-Sleep du CPU pendant les 5000 ms de calcul optique
    esp_sleep_enable_timer_wakeup(5000 * 1000ULL);
    esp_light_sleep_start();
  }

  uint16_t co2 = 0;
  float temp = 0.0f;
  float hum = 0.0f;
  bool isDataReady = false;

  for (int i = 0; i < 10; i++) {
    scd4x.getDataReadyFlag(isDataReady);
    if (isDataReady) break;
    delay(100);
  }

  if (isDataReady) {
    uint16_t readErr = scd4x.readMeasurement(co2, temp, hum);
    if (readErr != 0 || co2 < 350 || temp < -20.0f) {
      Serial.printf("Données SCD41 invalides ou erreur I2C (code %u, CO2: %u). Réessai dans 1 min.\n", readErr, co2);
      enterDeepSleep(60);
      return;
    }
    Serial.printf("CO2: %u ppm | Temp: %.1f °C | Hum: %.1f %% | Oiseau: %s\n",
                  co2, temp, hum, bird_is_down ? "BAS" : "HAUT");
  } else {
    Serial.println("Erreur de lecture SCD41. Nouvel essai dans 2 min.");
    enterDeepSleep(120);
    return;
  }

  // ── Machine à états officielle Birdie® ────────────────────────────────────
  uint64_t next_sleep_seconds = 600; // Par défaut : 10 minutes

  if (!bird_is_down) {
    // L'oiseau est actuellement EN HAUT
    if (co2 > 1000) {
      Serial.println("CO2 > 1000 ppm ! Descente de l'oiseau (Alerte).");
      moveBird(true);
      next_sleep_seconds = 120; // Alerte : 2 minutes
    } else {
      Serial.println("Air sain. Prochaine mesure dans 10 minutes.");
      next_sleep_seconds = 600;
    }
  } else {
    // L'oiseau est actuellement EN BAS
    if (co2 < 950) {
      Serial.println("CO2 < 950 ppm ! Fenêtre ouverte, l'oiseau remonte.");
      moveBird(false);
      next_sleep_seconds = 7200; // Cooldown : 2 heures
    } else {
      Serial.println("Air toujours vicié (>= 950 ppm). Maintien en bas.");
      next_sleep_seconds = 120;
    }
  }

  enterDeepSleep(next_sleep_seconds);
}

void loop() {
  // Vide
}
