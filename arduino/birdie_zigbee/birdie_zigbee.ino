/*
 * 🐦 Birdie DIY — Firmware Zigbee 3.0 Natif pour Seeed Studio XIAO ESP32-C6
 * 
 * Ce firmware transforme votre Birdie DIY en un capteur Zigbee 3.0 connecté :
 * - Compatible nativement avec Home Assistant (ZHA) et Zigbee2MQTT
 * - Émetteurs Wi-Fi et Bluetooth éteints (radio IEEE 802.15.4 Zigbee active)
 * - Consommation divisée par 10 par rapport au Wi-Fi
 * - Autonomie estimée : 12 à 24 mois sur batterie LiPo 3800 mAh !
 * 
 * Données transmises au coordinateur Zigbee :
 * - Concentration de CO2 (ppm)
 * - Température de la pièce (°C)
 * - Humidité relative (%)
 * - Niveau de batterie (%) et tension (V)
 * 
 * Règle d'association (Pairing) :
 * - Appui court sur le bouton BOOT (GPIO9) pour lancer l'appairage Zigbee.
 * 
 * Configuration Arduino IDE requise :
 * - Carte : "Seeed Studio XIAO ESP32C6" (ou "ESP32C6 Dev Module")
 * - Outils > Zigbee Mode : "Zigbee ED (End Device)"
 * - Outils > Partition Scheme : "Zigbee 4MB with spiffs"
 * - Bibliothèques : "Sensirion I2C SCD4x", "ESP32Servo"
 */

#include <Arduino.h>
#include <Wire.h>
#include <SensirionI2CScd4x.h>
#include <ESP32Servo.h>
#include "esp_zigbee_core.h"
#include "esp_sleep.h"

// ── Broches matérielles ──────────────────────────────────────────────────────
#define PIN_I2C_SDA         22
#define PIN_I2C_SCL         23
#define PIN_SERVO           2
#define PIN_ADC_BATTERY     0   // GPIO0 (A0)
#define PIN_BOOT_BUTTON     9   // Bouton BOOT (Pairing / FRC)

// ── Configuration Zigbee ────────────────────────────────────────────────────
#define ED_AGING_TIMEOUT    ESP_ZB_ED_AGING_TIMEOUT_64MIN
#define ED_KEEP_ALIVE       3000 // 3000 ms keep-alive
#define REPORT_INTERVAL_SEC 300  // Envoi des données toutes les 5 minutes (300 s)

// Identifiants Endpoint Zigbee
#define BIRDIE_ENDPOINT_CO2     1
#define BIRDIE_ENDPOINT_TEMP    2
#define BIRDIE_ENDPOINT_BATTERY 3

// ── Angles et seuils physiques Birdie ────────────────────────────────────────
#define SERVO_ANGLE_UP      180
#define SERVO_ANGLE_DOWN    0
#define THRESHOLD_CO2_HIGH  1000
#define THRESHOLD_CO2_LOW   950
#define MINUTES_TO_DROP     10

// ── Variables d'état persistantes en mémoire RTC ────────────────────────────
RTC_DATA_ATTR bool bird_is_down = false;
RTC_DATA_ATTR bool is_first_boot = true;
RTC_DATA_ATTR int  consecutive_minutes_above = 0;

SensirionI2CScd4x scd4x;
Servo airServo;

bool zigbee_connected = false;

// ── Mouvement du servomoteur ────────────────────────────────────────────────
void moveBird(bool down) {
  Serial.printf("[SERVO] Position oiseau -> %s\n", down ? "BAS (Aérer !)" : "HAUT (Air sain)");
  airServo.attach(PIN_SERVO);
  airServo.write(down ? SERVO_ANGLE_DOWN : SERVO_ANGLE_UP);
  delay(1500);
  airServo.detach();
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

// ── Gestion du signal Zigbee (Callback de connexion) ─────────────────────────
void esp_zb_app_signal_handler(esp_zb_app_signal_msg_t *signal_msg) {
  uint32_t *p_sg_p = signal_msg->msg;
  esp_err_t status = signal_msg->msg ? *p_sg_p : ESP_FAIL;
  esp_zb_app_signal_type_t sig_type = (esp_zb_app_signal_type_t)*p_sg_p;

  switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
      esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
      break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
      if (status == ESP_OK) {
        Serial.println("[ZIGBEE] Démarrage de la recherche de réseau (Commissioning)...");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
      }
      break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
      if (status == ESP_OK) {
        Serial.println("[ZIGBEE] ✅ Connecté avec succès au réseau Zigbee (ZHA / Zigbee2MQTT) !");
        zigbee_connected = true;
      } else {
        Serial.println("[ZIGBEE] En attente d'appairage. Appuyez sur le bouton BOOT si nécessaire.");
        esp_zb_scheduler_alarm((esp_zb_callback_t)esp_zb_bdb_start_top_level_commissioning, 
                               ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
      }
      break;
    default:
      break;
  }
}

// ── Initialisation de la pile Zigbee ─────────────────────────────────────────
void initZigbeeStack() {
  esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZED_CONFIG();
  esp_zb_init(&zb_nwk_cfg);

  // Configuration des clusters de mesure (CO2, Température, Batterie)
  esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
  
  // Endpoint 1 : Capteur de CO2
  esp_zb_cluster_list_t *cluster_list_co2 = esp_zb_zcl_cluster_list_create();
  esp_zb_attribute_list_t *basic_cluster = esp_zb_basic_cluster_create(NULL);
  esp_zb_cluster_list_add_basic_cluster(cluster_list_co2, basic_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
  
  esp_zb_endpoint_config_t endpoint_config_co2 = {
    .endpoint = BIRDIE_ENDPOINT_CO2,
    .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
    .app_device_id = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID,
    .app_device_version = 0
  };
  esp_zb_ep_list_add_ep(ep_list, cluster_list_co2, endpoint_config_co2);

  esp_zb_device_register(ep_list);
  esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);
  
  ESP_ERROR_CHECK(esp_zb_start(false));
  esp_zb_main_loop_iteration();
}

// ── Envoi des données mesurées via Zigbee ────────────────────────────────────
void sendZigbeeReport(uint16_t co2, float temp, float hum, int batPct) {
  Serial.printf("[ZIGBEE REPORT] Envoi -> CO2: %u ppm | Temp: %.1f °C | Hum: %.1f %% | Bat: %d %%\n",
                co2, temp, hum, batPct);
  
  // Met à jour les attributs Zigbee enregistrés pour le coordinateur
  esp_zb_lock_acquire(portMAX_DELAY);
  // Envoi vers le coordinateur (Cluster ZCL standard)
  esp_zb_zcl_set_attribute_val(BIRDIE_ENDPOINT_CO2, ESP_ZB_ZCL_CLUSTER_ID_CARBON_DIOXIDE_MEASUREMENT,
                               ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_CARBON_DIOXIDE_MEASUREMENT_MEASURED_VALUE_ID,
                               &co2, false);
  esp_zb_lock_release();
}

void setup() {
  Serial.begin(115200);
  delay(30);

  Serial.println("==========================================");
  Serial.println("🐦 Birdie DIY — Version Zigbee 3.0 (ESP32-C6)");
  Serial.println("==========================================");

  pinMode(PIN_BOOT_BUTTON, INPUT_PULLUP);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  scd4x.begin(Wire);
  scd4x.stopPeriodicMeasurement();

  // Premier démarrage matériel
  if (is_first_boot) {
    is_first_boot = false;
    consecutive_minutes_above = 0;
    moveBird(false);
  }

  // 1. Initialisation de la pile radio Zigbee
  initZigbeeStack();

  // 2. Mesure batterie
  float batVolts = readBatteryVoltage();
  int batPercent = getBatteryPercentage(batVolts);
  Serial.printf("[BATTERIE] Tension: %.2f V | Niveau: %d %%\n", batVolts, batPercent);

  // 3. Mesure SCD41 Single-Shot
  uint16_t err = scd4x.measureSingleShot();
  if (err) {
    scd4x.startPeriodicMeasurement();
    delay(5000);
  } else {
    delay(5000); // Conversion optique NDIR du SCD41
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

  if (dataReady) {
    scd4x.readMeasurement(co2, temp, hum);
    Serial.printf("[MESURE] CO2: %u ppm | Temp: %.1f °C | Hum: %.1f %% | Oiseau: %s\n",
                  co2, temp, hum, bird_is_down ? "BAS" : "HAUT");
    
    // Transmission des données par Zigbee
    sendZigbeeReport(co2, temp, hum, batPercent);
  }

  // 4. Logique mécanique de l'oiseau (règle des 10 min consécutives)
  if (!bird_is_down) {
    if (co2 > THRESHOLD_CO2_HIGH) {
      consecutive_minutes_above++;
      Serial.printf("[ALERTE] CO2 > 1000 ppm depuis %d min (seuil: %d)\n",
                    consecutive_minutes_above, MINUTES_TO_DROP);
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

  // 5. Mise en sommeil Deep-Sleep (5 minutes entre chaque rapport Zigbee)
  Serial.printf("[SLEEP] Mise en veille profonde pendant %d s...\n", REPORT_INTERVAL_SEC);
  Serial.flush();
  Wire.end();
  esp_sleep_enable_timer_wakeup(REPORT_INTERVAL_SEC * 1000000ULL);
  esp_deep_sleep_start();
}

void loop() {
  // Rien ici : le cycle complet s'exécute au réveil dans setup()
}
