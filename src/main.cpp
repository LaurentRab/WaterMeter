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
uint32_t lastReadMs[METER_COUNT] = {};
uint32_t lastLeakCheckMs    = 0;
uint32_t lastWatchdogMs     = 0;

// Structure des compteurs configurés
struct MeterCfg {
    uint32_t serial;
    uint8_t  year;
    float    freqMhz;  // fréquence de réponse propre au compteur
};
static const MeterCfg METERS[METER_COUNT] = {
    { METER_1_SERIAL, METER_1_YEAR, METER_1_FREQ_MHZ },
#if METER_COUNT >= 2
    { METER_2_SERIAL, METER_2_YEAR, METER_2_FREQ_MHZ },
#endif
#if METER_COUNT >= 3
    { METER_3_SERIAL, METER_3_YEAR, METER_3_FREQ_MHZ },
#endif
#if METER_COUNT >= 4
    { METER_4_SERIAL, METER_4_YEAR, METER_4_FREQ_MHZ },
#endif
};

// ============================================================
//  setup()
// ============================================================

#ifdef TUNE_FREQUENCY

// ============================================================
//  LED helpers — GPIO8 actif LOW (ESP32-C3 Super Mini)
// ============================================================

static void ledOn()  { digitalWrite(LED_PIN, LOW);  }
static void ledOff() { digitalWrite(LED_PIN, HIGH); }

// N blinks à 1 Hz (500 ms ON / 500 ms OFF)
static void ledBlink(int n) {
    for (int i = 0; i < n; i++) {
        ledOn();  delay(500);
        ledOff(); delay(500);
    }
}

// Heartbeat : 1 bref blink toutes les 2 s (attente / scan en cours)
static void ledHeartbeat() {
    ledOn(); delay(100); ledOff(); delay(1900);
}

// ============================================================
//  Résultats du scan par compteur
// ============================================================

struct TuneResult {
    bool        found;
    float       freqMhz;
    int8_t      rssi;
    EverBluData data;
};

// ============================================================
//  Boucle LED résultat (tourne indéfiniment après le scan)
//
//  N blinks lents = N compteurs trouvés
//  Si fréquences différentes : 1 flash court après les blinks
//  0 trouvé : clignotement rapide continu 5 Hz
// ============================================================

static void ledResultLoop(const TuneResult* results) {
    int   foundCount = 0;
    bool  sameFreq   = true;
    float firstFreq  = 0.0f;

    for (int i = 0; i < METER_COUNT; i++) {
        if (!results[i].found) continue;
        foundCount++;
        if (firstFreq == 0.0f) firstFreq = results[i].freqMhz;
        else if (fabsf(results[i].freqMhz - firstFreq) > 0.001f) sameFreq = false;
    }

    log_i("=== LED résultat : %d/%d trouvé(s)%s ===", foundCount, METER_COUNT,
          foundCount > 1 ? (sameFreq ? " — même fréq." : " — fréq. diff.") : "");

    while (true) {
        if (foundCount == 0) {
            // Aucune réponse → 5 Hz continu
            ledOn(); delay(100); ledOff(); delay(100);
        } else {
            ledBlink(foundCount);
            if (!sameFreq) {
                // Flash court = "fréquences différentes"
                delay(200);
                ledOn(); delay(150); ledOff();
            }
            delay(3000);
        }
    }
}

// ============================================================
//  Publication MQTT des résultats du scan
// ============================================================

static void publishTuneResults(const TuneResult* results) {
    char topic[80], payload[256];

    char timestamp[32] = "unknown";
    struct tm t;
    if (getLocalTime(&t, 0))
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &t);

    int foundCount = 0;
    for (int i = 0; i < METER_COUNT; i++)
        if (results[i].found) foundCount++;

    mqtt.publish("watermeter/tune/status",
                 foundCount > 0 ? "complete_found" : "complete_not_found", true);
    mqtt.publish("watermeter/tune/timestamp", timestamp, true);
    snprintf(payload, sizeof(payload), "%d/%d", foundCount, METER_COUNT);
    mqtt.publish("watermeter/tune/found_count", payload, true);

    for (int i = 0; i < METER_COUNT; i++) {
        if (METERS[i].serial == 0) continue;
        char serial[12];
        snprintf(serial, sizeof(serial), "%lu", METERS[i].serial);

        snprintf(topic, sizeof(topic), "watermeter/tune/%s/found", serial);
        mqtt.publish(topic, results[i].found ? "true" : "false", true);

        if (!results[i].found) continue;

        snprintf(topic, sizeof(topic), "watermeter/tune/%s/freq_mhz", serial);
        snprintf(payload, sizeof(payload), "%.3f", results[i].freqMhz);
        mqtt.publish(topic, payload, true);

        snprintf(topic, sizeof(topic), "watermeter/tune/%s/rssi", serial);
        snprintf(payload, sizeof(payload), "%d", (int)results[i].rssi);
        mqtt.publish(topic, payload, true);

        // Publier aussi la consommation sur le topic normal
        mqtt.publishEverBlu(METERS[i].serial, results[i].data, 0);
    }

    log_i("Résultats publiés → watermeter/tune/");
}

// ============================================================
//  runTuneFrequency()
//
//  1. WiFi + NTP + MQTT
//  2. (opt.) test séquences LED si TUNE_LED_TEST
//  3. Init CC1101
//  4. Attente fenêtre TUNE_HOUR_START–TUNE_HOUR_END / TUNE_DAYS_MASK
//  5. Scan 433.750–433.900 MHz, arrêt anticipé si tous trouvés
//  6. Publication MQTT
//  7. Boucle LED résultat (infinie)
// ============================================================

static void runTuneFrequency()
{
    const int   STEP_START = 0;
    const int   STEP_END   = 30;   // (433.900 - 433.750) / 0.005 = 30
    const float FREQ_BASE  = 433.750f;
    const float FREQ_STEP  = 0.005f;

    pinMode(LED_PIN, OUTPUT);
    ledOff();

    log_i("============================================");
    log_i("  MODE TuneFrequency");
    log_i("  Scan %.3f → %.3f MHz (pas 0.005)",
          FREQ_BASE + STEP_START * FREQ_STEP, FREQ_BASE + STEP_END * FREQ_STEP);
    log_i("  %d fréquences | fenêtre %02d:00–%02d:00 | masque jours 0x%02X",
          STEP_END - STEP_START + 1, TUNE_HOUR_START, TUNE_HOUR_END, TUNE_DAYS_MASK);
    log_i("============================================");

    // WiFi + MQTT + NTP
    mqtt.begin(WIFI_SSID, WIFI_PASSWORD);
    configTime(3600, 3600, "pool.ntp.org", "time.nist.gov");
    {
        struct tm t;
        uint32_t ts = millis();
        while (!getLocalTime(&t, 0) && millis() - ts < 10000) delay(500);
        if (getLocalTime(&t, 0))
            log_i("NTP synchro : %02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
        else
            log_w("NTP non synchro");
    }

    // ---- Test séquences LED (environnement tune_led_test) ----
#ifdef TUNE_LED_TEST
    log_i("=== TEST LED : toutes les séquences ===");
    log_i("1. Heartbeat (attente)...");
    for (int i = 0; i < 4; i++) ledHeartbeat();

    log_i("2. Aucune réponse (5 Hz rapide)...");
    { uint32_t t0 = millis(); while (millis() - t0 < 3000) { ledOn(); delay(100); ledOff(); delay(100); } }
    delay(1000);

    for (int n = 1; n <= METER_COUNT; n++) {
        log_i("3.%d. %d compteur(s), même fréquence...", n, n);
        for (int r = 0; r < 3; r++) { ledBlink(n); delay(3000); }

        log_i("3.%d. %d compteur(s), fréquences différentes...", n, n);
        for (int r = 0; r < 3; r++) {
            ledBlink(n);
            delay(200); ledOn(); delay(150); ledOff();
            delay(3000);
        }
    }
    log_i("=== TEST LED terminé — lancement du scan ===");
#endif

    // ---- Init CC1101 ----------------------------------------
    if (!radio.begin()) {
        log_e("CC1101 non détecté — abandon.");
        // Signal d'erreur : SOS (3 courts, 3 longs, 3 courts)
        while (true) {
            for (int i=0;i<3;i++){ledOn();delay(200);ledOff();delay(200);}
            for (int i=0;i<3;i++){ledOn();delay(600);ledOff();delay(200);}
            for (int i=0;i<3;i++){ledOn();delay(200);ledOff();delay(200);}
            delay(2000);
        }
    }
    radio.configureEverBlu();
    if (!radio.selfTest()) {
        log_e("Self-test CC1101 échoué — abandon.");
        while (true) { ledOn(); delay(100); ledOff(); delay(100); }  // 5 Hz
    }

    // ---- Attente fenêtre de scan ----------------------------
    {
        struct tm t;
        uint32_t lastLogMs = 0;
        bool inWindow = false;

        while (!inWindow) {
            mqtt.loop();
            if (getLocalTime(&t, 0)) {
                bool rightTime = (t.tm_hour >= TUNE_HOUR_START && t.tm_hour < TUNE_HOUR_END);
                bool rightDay  = ((TUNE_DAYS_MASK >> t.tm_wday) & 1);
                inWindow = rightTime && rightDay;
            }
            if (!inWindow) {
                if (millis() - lastLogMs > 60000UL) {
                    struct tm tc; getLocalTime(&tc, 0);
                    log_i("Attente fenêtre %02d:00–%02d:00 (actuellement %02d:%02d)",
                          TUNE_HOUR_START, TUNE_HOUR_END, tc.tm_hour, tc.tm_min);
                    lastLogMs = millis();
                }
                ledHeartbeat();  // 1 blink lent toutes les 2 s
            }
        }
        log_i("Fenêtre atteinte : %02d:%02d — démarrage scan", t.tm_hour, t.tm_min);
        mqtt.publish("watermeter/tune/status", "scanning", true);
    }

    // ---- Scan -----------------------------------------------
    TuneResult results[METER_COUNT] = {};

    for (int s = STEP_START; s <= STEP_END; s++) {
        float freq = FREQ_BASE + s * FREQ_STEP;
        log_i("--- %.3f MHz (%d/%d) ---", freq, s - STEP_START + 1, STEP_END - STEP_START + 1);

        // Bref blink = "alive"
        ledOn(); delay(50); ledOff();

        radio.configureEverBlu();
        radio.setFrequency(freq);

        for (int i = 0; i < METER_COUNT; i++) {
            if (METERS[i].serial == 0 || results[i].found) continue;

            EverBluData data;
            if (everblu.request(METERS[i].serial, METERS[i].year, data)) {
                log_i(">>> SUCCES compteur %d : %.3f MHz | %lu L | batt=%u | RSSI=%d <<<",
                      i+1, freq, data.liters, data.battery, data.rssi);
                log_i("    → METER_%d_FREQ_MHZ  %.3ff", i+1, freq);
                results[i].found   = true;
                results[i].freqMhz = freq;
                results[i].rssi    = data.rssi;
                results[i].data    = data;
            }
        }

        // Arrêt anticipé si tous les compteurs ont répondu
        bool allFound = true;
        for (int i = 0; i < METER_COUNT; i++)
            if (METERS[i].serial != 0 && !results[i].found) { allFound = false; break; }
        if (allFound) { log_i("Tous les compteurs trouvés — scan terminé."); break; }
    }

    // ---- Publication MQTT -----------------------------------
    publishTuneResults(results);

    // ---- Boucle LED résultat (infinie) ----------------------
    ledResultLoop(results);
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

    // Initialisation CC1101
    if (!radio.begin()) {
        log_e("CC1101 non détecté — vérifier le câblage !");
    }
    radio.configureEverBlu();
    radio.selfTest();

    // WiFi + MQTT (WiFi nécessaire avant configTime)
    mqtt.begin(WIFI_SSID, WIFI_PASSWORD);

    // NTP : configurer après connexion WiFi
    configTime(3600, 3600, "pool.ntp.org", "time.nist.gov");  // UTC+1 (ajuste selon saison)
    {
        struct tm t;
        uint32_t ts = millis();
        while (!getLocalTime(&t, 0) && millis() - ts < 10000) delay(500);
        if (getLocalTime(&t, 0))
            log_i("NTP synchro : %02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
        else
            log_w("NTP non synchro — fenêtre horaire ignorée jusqu'à synchro");
    }

    for (int i = 0; i < METER_COUNT; i++)
        log_i("Compteur %d : serial=%lu année=%u freq=%.3f MHz",
              i+1, METERS[i].serial, METERS[i].year, METERS[i].freqMhz);
    log_i("Intervalle : %u min | Fenêtre : 06:00–18:59", READ_INTERVAL_MIN);

    // Première lecture immédiate (sans attendre l'intervalle)
    for (int i = 0; i < METER_COUNT; i++)
        lastReadMs[i] = millis() - (uint32_t)READ_INTERVAL_MIN * 60000UL;
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
    for (int i = 0; i < METER_COUNT; i++) {
        if (METERS[i].serial == 0) continue;          // Compteur non configuré
        if (now - lastReadMs[i] < intervalMs) continue; // Pas encore l'heure

        if (!EverBlu::withinTimeWindow()) {
            // Hors fenêtre : on reporte sans log répété
            lastReadMs[i] = now;
            continue;
        }

        log_i("--- Interrogation compteur %d (serial=%lu, %.3f MHz) ---",
              i + 1, METERS[i].serial, METERS[i].freqMhz);
        radio.setFrequency(METERS[i].freqMhz);

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
