# WaterMeter — Itron EverBlu Cyble Enhanced → Home Assistant

Lecture automatique des compteurs d'eau **Itron EverBlu Cyble Enhanced (SEDIF / Actaris P40)** via radio 433 MHz, publiée en MQTT vers **Home Assistant**.

## Matériel

| Composant | Référence |
|-----------|-----------|
| Microcontrôleur | ESP32-C3 Super Mini |
| Module radio | CC1101 (433 MHz) |

### Câblage CC1101 ↔ ESP32-C3

| CC1101 | ESP32-C3 | Notes |
|--------|----------|-------|
| GDO0   | GPIO 4   | |
| CSN    | GPIO 5   | |
| SCK    | GPIO 6   | |
| MOSI   | GPIO 7   | |
| MISO   | **GPIO 10**  | GPIO 8 réservé à la LED intégrée |
| VCC    | 3.3 V    | |
| GND    | GND      | |

> **Note câblage** : GPIO 8 est la LED bleue intégrée de l'ESP32-C3 Super Mini (active LOW).
> Le fil MISO du CC1101 doit être connecté sur **GPIO 10** (et non GPIO 8).

## Protocole

- **Fréquence** : 433.82 MHz (nominale — varie par compteur, voir TuneFrequency)
- **Modulation** : 2-FSK · 2.4 kbps · déviation 5.157 kHz
- **Séquence** : wake-up ~2 s → requête 39 octets → ACK 18 octets → données 4× oversampled
- **Fenêtre active** : les compteurs ne répondent qu'entre **06:00 et 18:59**

## Configuration

Copier le fichier d'exemple puis renseigner vos valeurs :

```bash
cp include/config.example.h include/config.h
```

Editer `include/config.h` avec vos identifiants WiFi, MQTT et les numéros de série de vos compteurs. Ce fichier est ignoré par git (secrets).

## Compilation & flash

Projet **PlatformIO**. Environnements disponibles :

| Environnement | Usage |
|---------------|-------|
| `watermeter` | Firmware normal (production) |
| `tune` | Scan de fréquence (calibration) |

```bash
# Firmware normal
pio run -e watermeter --target upload

# Mode TuneFrequency
pio run -e tune --target upload
```

Depuis **VSCode** : `Ctrl+Shift+P` → *Tasks: Run Task* → choisir la tâche souhaitée.

## Mode TuneFrequency

Chaque compteur peut avoir une légère dérive fréquentielle (plage constatée : 433.76 – 433.89 MHz). Le mode `tune` scanne automatiquement **433.750 → 433.900 MHz par pas de 5 kHz** (31 fréquences) et affiche la fréquence à laquelle le compteur répond.

```
>>> ✓ SUCCES — fréquence OK : 433.820 MHz <<<
    → Mettre  #define CC1101_FREQ_MHZ  433.820f  dans config.h
```

Recopier la valeur dans `config.h`, puis recompiler avec l'environnement `watermeter`.

## Topics MQTT

| Topic | Contenu |
|-------|---------|
| `watermeter/<serial>/state` | JSON `{ liters, battery, rssi, timestamp }` |
| `watermeter/<serial>/leak` | `"ON"` / `"OFF"` |

Home Assistant Auto-Discovery est activé — les entités apparaissent automatiquement.

## Détection de fuite

Une alerte MQTT (`leak = ON`) est publiée si la consommation dépasse `LEAK_THRESHOLD_L` litres durant la période calme (`LEAK_QUIET_HOUR_START` – `LEAK_QUIET_HOUR_END`, par défaut 00:00–05:00).

## Watchdog

Un warning est publié si un compteur ne répond pas pendant plus de `WATCHDOG_TIMEOUT_MS` ms (défaut : 2 heures).

## Structure du projet

```
├── include/
│   ├── config.example.h  — template de configuration (versionné)
│   └── config.h          — paramètres utilisateur (ignoré par git)
├── lib/
│   ├── CC1101/            — driver SPI bas niveau
│   ├── EverBlu/           — protocole Itron EverBlu
│   └── MQTTManager/       — WiFi + MQTT + HA discovery
├── src/
│   └── main.cpp           — boucle principale + mode TuneFrequency
└── platformio.ini
```

## Crédits

Protocole EverBlu basé sur le reverse-engineering de [hallard/psykokwak](https://github.com/psykokwak-com/everblu-meters-esp8266).
