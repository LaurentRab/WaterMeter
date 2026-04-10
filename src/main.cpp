#include <Arduino.h>
#include "config.h"
#include "CC1101.h"
#include "EverBlu.h"
#include "MQTTManager.h"

// Mode TuneFrequency : compiler avec  pio run -e tune  (défini via platformio.ini)
// Ne PAS ajouter #define TUNE_FREQUENCY ici — utiliser l'environnement tune.

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

#ifdef TUNE_FREQUENCY
// Scan des fréquences 433.750 → 433.900 MHz par pas de 0.005 MHz (5 kHz).
// Lance pio run -e tune --target upload pour compiler ce mode.
static void runTuneFrequency()
{
    // Utilise des pas entiers pour éviter la dérive flottante
    // step 0 = 433.750, step 1 = 433.755, ..., step 30 = 433.900
    const int   STEP_START = 0;
    const int   STEP_END   = 30;   // (433.900 - 433.750) / 0.005 = 30
    const float FREQ_BASE  = 433.750f;
    const float FREQ_STEP  = 0.005f;

    log_i("============================================");
    log_i("  MODE TuneFrequency");
    log_i("  Scan %.3f → %.3f MHz (pas 0.005)",
          FREQ_BASE + STEP_START * FREQ_STEP,
          FREQ_BASE + STEP_END   * FREQ_STEP);
    log_i("  %d fréquences à tester", STEP_END - STEP_START + 1);
    log_i("  Attention : compteurs actifs de 06:00 à 18:59");
    log_i("============================================");

    if (!radio.begin()) {
        log_e("CC1101 non détecté — vérifier le câblage !");
        return;
    }
    radio.configureEverBlu();
    if (!radio.selfTest()) {
        log_e("Self-test échoué — abandon du scan.");
        return;
    }

    bool found = false;

    for (int s = STEP_START; s <= STEP_END; s++) {
        float freq = FREQ_BASE + s * FREQ_STEP;
        log_i("========== *******  ==========");
        log_i("--- Test %.3f MHz (%d/%d) ---", freq, s - STEP_START + 1, STEP_END - STEP_START + 1);

        // Reconfigurer complètement puis surcharger la fréquence
        radio.configureEverBlu();
        radio.setFrequency(freq);

        for (int i = 0; i < 2; i++) {
            if (METERS[i].serial == 0) continue;

            EverBluData data;
            if (everblu.request(METERS[i].serial, METERS[i].year, data)) {
                log_i(">>> ✓ SUCCES — fréquence OK : %.3f MHz <<<", freq);
                log_i("    Compteur %d : %lu L | batterie=%u mois | RSSI=%d dBm",
                      i + 1, data.liters, data.battery, data.rssi);
                log_i("    → Mettre  #define CC1101_FREQ_MHZ  %.3ff  dans config.h", freq);
                found = true;
            } else {
                log_i("  %.3f MHz — compteur %d : pas de réponse", freq, i + 1);
            }
        }
    }

    if (!found) {
        log_w("Aucune réponse sur toute la plage — vérifier câblage, fenêtre horaire et serial/année.");
    }

    log_i("=== Scan terminé ===");
}
#endif  // TUNE_FREQUENCY

void setup()
{
    delay(5000);  // Laisse le temps à l'USB-JTAG de s'énumérer

#ifdef TUNE_FREQUENCY
    runTuneFrequency();
    // Halt : pas de loop utile en mode tune
    while (true) delay(1000);
#else
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
    radio.selfTest();

    // WiFi + MQTT
    mqtt.begin(WIFI_SSID, WIFI_PASSWORD);

    log_i("Compteur 1 : serial=%lu année=%u", METERS[0].serial, METERS[0].year);
    log_i("Compteur 2 : serial=%lu année=%u", METERS[1].serial, METERS[1].year);
    log_i("Intervalle : %u min | Fenêtre : 06:00–18:59", READ_INTERVAL_MIN);

    // Première lecture immédiate (sans attendre l'intervalle)
    lastReadMs[0] = lastReadMs[1] = millis() - (uint32_t)READ_INTERVAL_MIN * 60000UL;
#endif  // TUNE_FREQUENCY
}

// ============================================================
//  loop()
// ============================================================

void loop()
{
#ifdef TUNE_FREQUENCY
    delay(1000);  // Unreachable — kept to satisfy linker
    return;
#endif

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
            mqtt.publishEverBlu(METERS[i].serial, data, LEAK_THRESHOLD_L);
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
        mqtt.checkLeaks();
    }

    // --- Watchdog (toutes les heures) -----------------------
    if (now - lastWatchdogMs > 3600000UL) {
        lastWatchdogMs = now;
        mqtt.checkWatchdog(WATCHDOG_TIMEOUT_MS);
    }

    delay(100);
}
