#pragma once

// ============================================================
//  CONFIGURATION — copier ce fichier vers config.h et renseigner
//  vos valeurs avant compilation.
//
//    cp include/config.example.h include/config.h
// ============================================================

// --- WiFi ---------------------------------------------------
#define WIFI_SSID     "MonReseau"
#define WIFI_PASSWORD "MonMotDePasse"

// --- MQTT (broker Home Assistant) ---------------------------
#define MQTT_SERVER    "192.168.x.x"
#define MQTT_PORT      1883
#define MQTT_USER      "mqtt-user"
#define MQTT_PASS      "mqtt-pass"
#define MQTT_CLIENT_ID "watermeter_esp32"
#define MQTT_BASE_TOPIC "watermeter"

// --- Compteurs Itron EverBlu Cyble Enhanced ------------------
//
//  Le numero imprime sur le module suit le format :
//    [2 chiffres usine][6 chiffres serial][1 chiffre controle]
//  Exemple : 843561553 -> usine=84, serial=356155, controle=3
//
//  METER_x_SERIAL = les 6 chiffres centraux
//  METER_x_YEAR   = les 2 derniers chiffres de l'annee de fabrication
//                   (ex: 19 pour 2019, indique par "19399" sur l'etiquette)
//
//  Si aucune reponse : essaie METER_x_SERIAL avec les 8 chiffres
//  sans le controle (ex: 84356155UL) — certains modules utilisent
//  un format different.
//
#define METER_1_SERIAL  0UL        // 0 = desactive
#define METER_1_YEAR    0

#define METER_2_SERIAL  0UL        // 0 = desactive
#define METER_2_YEAR    0

// --- Frequence CC1101 ----------------------------------------
// Valeur nominale EverBlu : 433.82 MHz
// Chaque compteur peut avoir une legere derive : [433.76 - 433.89]
// Ajuste avec le mode TuneFrequency si aucune reponse.
#define CC1101_FREQ_MHZ  433.82f

// --- Interrogation -------------------------------------------
// Intervalle entre deux lectures (minutes)
// Le compteur ne repond qu'entre 06:00 et 18:59 !
#define READ_INTERVAL_MIN  60

// --- Broches CC1101 <-> ESP32-C3 Super Mini ------------------
#define CC1101_GDO0   4
#define CC1101_CSN    5
#define CC1101_SCK    6
#define CC1101_MOSI   7
#define CC1101_MISO   8

// --- Detection de fuite -------------------------------------
// Consommation nocturne maximale toleree (litres).
#define LEAK_THRESHOLD_L  20

// Watchdog : log warning si un compteur est silencieux > N ms
#define WATCHDOG_TIMEOUT_MS  7200000UL  // 2 heures
