/*
 * 🐦 Birdie DIY — Version Arduino C++ Standalone v2 (Ultra-Basse Consommation)
 * 
 * 100% Hors-Ligne : ZÉRO Wi-Fi, ZÉRO Bluetooth, ZÉRO Cloud.
 * 
 * Fonctionnalités intégrées :
 * 1. Logique officielle v2 (Filtre 10 minutes consécutives > 1000 ppm avant descente)
 * 2. Remontée immédiate sous 950 ppm (hystérésis)
 * 3. 🔋 Gestion de batterie (lecture ADC GPIO0, courbe LiPo, protection décharge profonde)
 * 4. 🪫 Alerte visuelle "Batterie Faible" via le servo (< 3.4V / 15%)
 * 5. 🎯 Calibration forcée FRC (400 ppm) via appui 5s sur le bouton BOOT (GPIO9)
 * 6. ⚡ Optimisation Light-Sleep CPU (~1.5 mA pendant les 5s de conversion SCD41)
 * 7. 💤 Deep-Sleep matériel (< 10 µA)
 * 
 * Câblage XIAO ESP32-C6 :
 * - SDA    : GPIO22 (D4)
 * - SCL    : GPIO23 (D5)
 * - SG90   : GPIO2  (D2)
 * - BUZZER : GPIO21 (D3 - fil + vers D3, fil - vers GND) [Optionnel]
 * - BAT+   : GPIO0  (A0 - Point milieu pont 100k/100k)
 * - BOOT   : GPIO9  (Bouton intégré sur la carte XIAO)
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

// ── Broches matérielles ──────────────────────────────────────────────────────
#define PIN_I2C_SDA         22
#define PIN_I2C_SCL         23
#define PIN_SERVO           2
#define PIN_BUZZER          21  // Broche D3 (GPIO21 sur XIAO ESP32-C6) pour buzzer passif
#define PIN_ADC_BATTERY     0   // GPIO0 (A0 sur XIAO)
#define PIN_BOOT_BUTTON     9   // Bouton BOOT de la carte XIAO ESP32-C6

// ── Options configurables ───────────────────────────────────────────────────
#define ENABLE_BUZZER       true // Mettre à false pour désactiver le son

// ── Angles du servomoteur SG90 ──────────────────────────────────────────────
#define SERVO_ANGLE_UP      180 // Oiseau dressé en haut (air sain)
#define SERVO_ANGLE_DOWN    0   // Oiseau penché en bas (aérer !)
#define SERVO_ANGLE_MID     90  // Position intermédiaire pour alerte batterie

// ── Seuils CO2 (ppm) ─────────────────────────────────────────────────────────
#define THRESHOLD_CO2_HIGH  1000 // Seuil d'alerte haute (ppm)
#define THRESHOLD_CO2_LOW   950  // Seuil de rétablissement (hystérésis)
#define MINUTES_REQUIRED_FOR_DROP 10 // Minutes consécutives avant descente

// ── Seuils Batterie LiPo (Volts) ─────────────────────────────────────────────
#define BATTERY_LOW_VOLTS   3.40f // Seuil alerte batterie faible (~15%)
#define BATTERY_CRIT_VOLTS  3.25f // Seuil critique : sommeil sécurité (~3%)

// ── Intervalles de veille ────────────────────────────────────────────────────
// SMART_ECO_MODE :
// - false : Réveil strict chaque minute (60s). Autonomie ~8-9 mois.
// - true  : Réveil 5 min si air sain, bascule à 1 min si CO2 > 1000 ppm. Autonomie ~16-18 mois.
#define SMART_ECO_MODE      true

#define INTERVAL_NORMAL_AIR_SEC   300 // 5 minutes en air sain (Mode Éco)
#define INTERVAL_CHECK_ALERT_SEC  60  // 1 minute pendant le comptage alerte
#define INTERVAL_RECOVERY_SEC     120 // 2 minutes quand l'oiseau est en bas

// Mettre à 1 pour activer le Light-Sleep du CPU pendant les 5s de mesure du SCD41
#define USE_LIGHT_SLEEP     1

// ── Variables persistantes en mémoire RTC (survivent au Deep-Sleep) ──────────
RTC_DATA_ATTR bool bird_is_down = false;
RTC_DATA_ATTR bool is_first_boot = true;
RTC_DATA_ATTR int  consecutive_minutes_above = 0;
RTC_DATA_ATTR bool low_battery_warned = false; // Évite de répéter l'animation à chaque réveil

SensirionI2CScd4x scd4x;
Servo airServo;

// ── Gazouillis d'oiseau mélodieux via le buzzer PWM ──────────────────────────
void playBirdChirp() {
#if ENABLE_BUZZER
  pinMode(PIN_BUZZER, OUTPUT);
  // Gazouillis ascendant doux (glissando rapide de fréquences)
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

// ── Gestion du servomoteur ──────────────────────────────────────────────────
void moveBird(bool down) {
  Serial.printf("[SERVO] Mouvement oiseau -> %s\n", down ? "BAS (Alerte)" : "HAUT (Air pur)");
  airServo.attach(PIN_SERVO, 500, 2500);
  airServo.write(down ? SERVO_ANGLE_DOWN : SERVO_ANGLE_UP);
  delay(2500); // Laisse 2.5s au moteur pour terminer sa rotation complète sous charge
  airServo.detach(); // Coupe le PWM pour supprimer tout courant de maintien
  bird_is_down = down;
}

// ── Animation alerte visuelle batterie faible ────────────────────────────────
void alertLowBatteryVisual() {
  Serial.println("[BATTERIE] 🪫 ALERTE : Batterie faible ! Animation distinctive du servo...");
  airServo.attach(PIN_SERVO);
  
  // Petite animation : monte à 90°, descend à 45°, remonte à 90°
  airServo.write(SERVO_ANGLE_MID);
  delay(500);
  airServo.write(45);
  delay(400);
  airServo.write(SERVO_ANGLE_MID);
  delay(400);
  
  // Reprend la position normale selon l'état actuel de l'air
  airServo.write(bird_is_down ? SERVO_ANGLE_DOWN : SERVO_ANGLE_UP);
  delay(1000);
  airServo.detach();
}

// ── Mesure de la tension batterie ────────────────────────────────────────────
float readBatteryVoltage() {
  // analogReadMilliVolts compense la calibration eFuse de l'ESP32-C6
  uint32_t mv = analogReadMilliVolts(PIN_ADC_BATTERY);
  // Pont diviseur 100k / 100k (facteur 2.0)
  float voltage = (mv * 2.0f) / 1000.0f;
  return voltage;
}

// ── Calcul du pourcentage LiPo (courbe réaliste non-linéaire) ────────────────
int getBatteryPercentage(float v) {
  if (v >= 4.15f) return 100;
  if (v <= 3.25f) return 0;
  if (v > 3.80f)  return 60 + (int)((v - 3.80f) * (40.0f / (4.15f - 3.80f)));
  if (v > 3.65f)  return 20 + (int)((v - 3.65f) * (40.0f / (3.80f - 3.65f)));
  return (int)((v - 3.25f) * (20.0f / (3.65f - 3.25f)));
}

// ── Calibration forcée FRC (400 ppm) via appui long bouton BOOT ──────────────
void checkAndHandleCalibration() {
  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);
  
  // Si le bouton BOOT est pressé au démarrage
  if (digitalRead(PIN_BOOT_BUTTON) == LOW) {
    Serial.println("[CALIBRATION] Bouton BOOT détecté ! Maintenez 5s pour lancer la calibration FRC 400 ppm...");
    unsigned long start = millis();
    bool held = true;
    while (millis() - start < 5000) {
      if (digitalRead(PIN_BOOT_BUTTON) != LOW) {
        held = false;
        Serial.println("[CALIBRATION] Appui relâché avant 5 secondes. Démarrage normal.");
        break;
      }
      delay(100);
    }
    
    if (held) {
      Serial.println("\n[CALIBRATION] 🎯 Lancement de la calibration forcée à 400 ppm...");
      // Arrêt d'une éventuelle mesure
      scd4x.stopPeriodicMeasurement();
      delay(500);
      
      uint16_t frcCorrection = 0;
      uint16_t err = scd4x.performForcedRecalibration(400, frcCorrection);
      
      if (err == 0 && frcCorrection != 0xFFFF) {
        Serial.printf("[CALIBRATION] ✅ Succès ! Décalage de correction appliqué: %d ppm\n", (int16_t)frcCorrection - 0x8000);
        scd4x.persistSettings(); // Enregistrement en mémoire flash non-volatile du SCD41
        
        // Confirmation visuelle : double battement de l'oiseau
        airServo.attach(PIN_SERVO);
        for (int i = 0; i < 2; i++) {
          airServo.write(180);
          delay(300);
          airServo.write(130);
          delay(300);
        }
        airServo.write(180);
        delay(500);
        airServo.detach();
      } else {
        Serial.printf("[CALIBRATION] ❌ Échec de calibration FRC (Code erreur: %u). Capteur non stabilisé.\n", err);
      }
    }
  }
}

// ── Mise en veille profonde (Deep-Sleep) ─────────────────────────────────────
void enterDeepSleep(uint64_t seconds) {
  Wire.end(); // Coupe le bus I2C avant l'endormissement
  esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
  Serial.printf("[SLEEP] Mise en Deep-Sleep pendant %llu s (< 10 µA)...\n\n", seconds);
  Serial.flush();
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(30);

  Serial.println("==========================================");
  Serial.println("🐦 Birdie DIY — Standalone v2 (Batterie & FRC)");
  Serial.println("==========================================");

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  scd4x.begin(Wire);

  // 1. Vérification d'une demande de calibration forcée
  checkAndHandleCalibration();

  // 2. Mesure et gestion de la batterie
  float batVolts = readBatteryVoltage();
  int batPercent = getBatteryPercentage(batVolts);
  Serial.printf("[BATTERIE] Tension: %.2f V | Niveau: %d %%\n", batVolts, batPercent);

  // Protection contre la décharge profonde (< 3.25V)
  if (batVolts < BATTERY_CRIT_VOLTS && batVolts > 1.0f) {
    Serial.println("[BATTERIE] 🛑 NIVEAU CRITIQUE ! Sommeil prolongé (1h) pour préserver l'accu.");
    moveBird(true); // Abaisse l'oiseau pour signaler la coupure
    enterDeepSleep(3600); // Dort 1h
    return;
  }

  // Alerte visuelle batterie faible (< 3.4V)
  if (batVolts < BATTERY_LOW_VOLTS && batVolts > 1.0f && !low_battery_warned) {
    alertLowBatteryVisual();
    low_battery_warned = true; // Ne rejoue pas l'animation à chaque minute
  } else if (batVolts >= BATTERY_LOW_VOLTS) {
    low_battery_warned = false; // Réinitialise l'alerte si rechargé
  }

  // 3. Initialisation au tout premier allumage physique
  if (is_first_boot) {
    is_first_boot = false;
    consecutive_minutes_above = 0;
    moveBird(false); // Dresse l'oiseau en position haute
  }

  // 4. Mesure SCD41 basse consommation (Single-Shot)
  scd4x.stopPeriodicMeasurement();
  uint16_t err = scd4x.measureSingleShot();
  if (err) {
    Serial.println("[SCD41] Déclenchement périodique secours...");
    scd4x.startPeriodicMeasurement();
    delay(5000);
  } else {
#if USE_LIGHT_SLEEP
    // Sommeil léger du CPU pendant les 5000 ms de calcul optique
    esp_sleep_enable_timer_wakeup(5000 * 1000ULL);
    esp_light_sleep_start();
#else
    delay(5000);
#endif
  }

  // Récupération des données du capteur
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
    enterDeepSleep(INTERVAL_CHECK_ALERT_SEC);
    return;
  }

  uint16_t readErr = scd4x.readMeasurement(co2, temp, hum);
  if (readErr != 0 || co2 < 350 || temp < -20.0f) {
    Serial.printf("[WARN] Données SCD41 invalides ou erreur I2C (code %u, CO2: %u). Réessai au prochain cycle.\n", readErr, co2);
    enterDeepSleep(INTERVAL_CHECK_ALERT_SEC);
    return;
  }
  Serial.printf("[DATA] CO2: %u ppm | Temp: %.1f °C | Hum: %.1f %% | Oiseau: %s\n",
                co2, temp, hum, bird_is_down ? "BAS" : "HAUT");

  // ── 5. Logique v2 : 10 minutes consécutives au-dessus de 1 000 ppm ──────────
  uint64_t next_sleep_sec = INTERVAL_CHECK_ALERT_SEC;

  if (!bird_is_down) {
    // ── Cas 1 : L'oiseau est en HAUT ──
    if (co2 > THRESHOLD_CO2_HIGH) {
      consecutive_minutes_above++;
      Serial.printf("[ALERTE] CO2 > 1000 ppm depuis %d min (Seuil: %d min)\n",
                    consecutive_minutes_above, MINUTES_REQUIRED_FOR_DROP);

      if (consecutive_minutes_above >= MINUTES_REQUIRED_FOR_DROP) {
        Serial.println("[ACTION] 10 minutes consécutives dépassées -> Descente de l'oiseau + Gazouillis !");
        playBirdChirp(); // Gazouillis sonore d'alerte discret
        moveBird(true);
        next_sleep_sec = INTERVAL_RECOVERY_SEC;
      } else {
        next_sleep_sec = INTERVAL_CHECK_ALERT_SEC;
      }
    } else {
      if (consecutive_minutes_above > 0) {
        Serial.println("[INFO] Air revenu sous 1000 ppm. Compteur réinitialisé.");
      }
      consecutive_minutes_above = 0;

#if SMART_ECO_MODE
      next_sleep_sec = INTERVAL_NORMAL_AIR_SEC;
#else
      next_sleep_sec = INTERVAL_CHECK_ALERT_SEC;
#endif
    }
  } else {
    // ── Cas 2 : L'oiseau est en BAS ──
    if (co2 < THRESHOLD_CO2_LOW) {
      Serial.printf("[ACTION] CO2 repassé sous %d ppm (%u ppm) -> Remontée de l'oiseau !\n",
                    THRESHOLD_CO2_LOW, co2);
      moveBird(false);
      consecutive_minutes_above = 0;
#if SMART_ECO_MODE
      next_sleep_sec = INTERVAL_NORMAL_AIR_SEC;
#else
      next_sleep_sec = INTERVAL_CHECK_ALERT_SEC;
#endif
    } else {
      Serial.printf("[INFO] Air toujours pollué (%u ppm >= %d ppm). Maintien en bas.\n",
                    co2, THRESHOLD_CO2_LOW);
      next_sleep_sec = INTERVAL_RECOVERY_SEC;
    }
  }

  enterDeepSleep(next_sleep_sec);
}

void loop() {
  // Vide : le microcontrôleur s'endort en Deep-Sleep à la fin du setup()
}
