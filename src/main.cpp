#include <Arduino.h>
#include "config.h"
#include "CC1101.h"
#include "EverBlu.h"
#include "MQTTManager.h"

// ============================================================
//  WaterMeter — ESP32-C3 + CC1101
//  Itron EverBlu Cyble Enhanced (SEDIF) → Home Assistant MQTT
//
//  Protocole : 433.82 MHz · 2-FSK · 2.4 kbps
//  Interrogation active toutes les READ_INTERVAL_MIN minutes
//  Fenêtre autorisée : 06:00 – 18:59
// ============================================================

CC1101     radio(CC1101_CSN, CC1101_GDO0, CC1101_SCK, CC1101_MOSI, CC1101_MISO);
EverBlu    everblu(radio);
MQTTManager mqtt(MQTT_SERVER, MQTT_PORT, MQTT_USER, MQTT_PASS,
                 MQTT_CLIENT_ID, MQTT_BASE_TOPIC);

// Dernière interrogation réussie par compteur (millis)
uint32_t lastReadMs[2]      = {0, 0};
uint32_t lastLeakCheckMs    = 0;
uint32_t lastWatchdogMs     = 0;

// Structure des compteurs configurés
struct MeterCfg {
    uint32_t serial;
    uint8_t  year;
};
static const MeterCfg METERS[2] = {
    { METER_1_SERIAL, METER_1_YEAR },
    { METER_2_SERIAL, METER_2_YEAR },
};

// ============================================================
//  setup()
// ============================================================

void setup()
{
    delay(5000);  // Laisse le temps à l'USB-JTAG de s'énumérer
    log_i("==============================");
    log_i("  WaterMeter v2.0  EverBlu");
    log_i("  ESP32-C3 + CC1101 433.82 MHz");
    log_i("==============================");

    // NTP pour la détection de fuite et la fenêtre horaire
    configTime(3600, 3600, "pool.ntp.org", "time.nist.gov");  // UTC+1 (ajuste selon saison)

    // Initialisation CC1101
    if (!radio.begin()) {
        log_e("CC1101 non détecté — vérifier le câblage !");
    }
    radio.configureEverBlu();

    // WiFi + MQTT
    mqtt.begin(WIFI_SSID, WIFI_PASSWORD);

    log_i("Compteur 1 : serial=%lu année=%u", METERS[0].serial, METERS[0].year);
    log_i("Compteur 2 : serial=%lu année=%u", METERS[1].serial, METERS[1].year);
    log_i("Intervalle : %u min | Fenêtre : 06:00–18:59", READ_INTERVAL_MIN);

    // Première lecture immédiate (sans attendre l'intervalle)
    lastReadMs[0] = lastReadMs[1] = millis() - (uint32_t)READ_INTERVAL_MIN * 60000UL;
}

// ============================================================
//  loop()
// ============================================================

void loop()
{
    mqtt.loop();

    uint32_t now = millis();
    uint32_t intervalMs = (uint32_t)READ_INTERVAL_MIN * 60000UL;

    // --- Interrogation périodique de chaque compteur --------
    for (int i = 0; i < 2; i++) {
        if (METERS[i].serial == 0) continue;          // Compteur non configuré
        if (now - lastReadMs[i] < intervalMs) continue; // Pas encore l'heure

        if (!EverBlu::withinTimeWindow()) {
            // Hors fenêtre : on reporte sans log répété
            lastReadMs[i] = now;
            continue;
        }

        log_i("--- Interrogation compteur %d (serial=%lu) ---", i + 1, METERS[i].serial);

        EverBluData data;
        if (everblu.request(METERS[i].serial, METERS[i].year, data)) {
            log_i("Compteur %d : %lu L | batterie=%u mois | RSSI=%d dBm | lectures=%u",
                  i + 1, data.liters, data.battery, data.rssi, data.readCount);
            mqtt.publishEverBlu(METERS[i].serial, data);
        } else {
            log_w("Compteur %d : aucune réponse", i + 1);
        }

        lastReadMs[i] = now;

        // Petite pause entre les deux compteurs pour ne pas se chevaucher
        if (i == 0) delay(2000);
    }

    // --- Détection de fuite (toutes les 5 min) --------------
    if (now - lastLeakCheckMs > 300000UL) {
        lastLeakCheckMs = now;
        mqtt.checkLeaks(LEAK_QUIET_HOUR_START, LEAK_QUIET_HOUR_END, LEAK_THRESHOLD_L);
    }

    // --- Watchdog (toutes les heures) -----------------------
    if (now - lastWatchdogMs > 3600000UL) {
        lastWatchdogMs = now;
        mqtt.checkWatchdog(WATCHDOG_TIMEOUT_MS);
    }

    delay(100);
}
