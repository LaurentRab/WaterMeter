#include <Arduino.h>
#include <time.h>
#include "EverBlu.h"

// ============================================================
//  Constantes protocole
// ============================================================

// Préfixe de synchronisation TX (9 octets, envoyés non encodés)
static const uint8_t SYNC_PREFIX[9] = {
    0x50, 0x00, 0x00, 0x00, 0x03, 0xFF, 0xFF, 0xFF, 0xFF
};

// Template de payload requête (17 octets + 2 octets CRC)
static const uint8_t REQ_TEMPLATE[17] = {
    0x13, 0x10, 0x00, 0x45,
    0x00,             // [4]  : année (rempli dynamiquement)
    0x00, 0x00, 0x00, // [5-7]: serial 3 octets MSB-first (rempli dynamiquement)
    0x00, 0x45, 0x20, 0x0A, 0x50, 0x14, 0x00, 0x0A  // [8-15]: champ fixe
    // [16]: dernier octet fixe
};

// ============================================================
//  Constructeur
// ============================================================

EverBlu::EverBlu(CC1101& radio) : _radio(radio) {}

// ============================================================
//  Fenêtre horaire autorisée : 06:00 – 18:59
// ============================================================

bool EverBlu::withinTimeWindow()
{
    struct tm t;
    if (!getLocalTime(&t, 0)) return true;  // NTP non synchro → on tente quand même
    return (t.tm_hour >= 6 && t.tm_hour < 19);
}

// ============================================================
//  Requête principale
// ============================================================

bool EverBlu::request(uint32_t serial, uint8_t year, EverBluData& out)
{
    out.valid = false;

    // Construction de la trame de requête
    uint8_t reqBuf[48];
    uint8_t reqLen = _buildRequest(serial, year, reqBuf);
    log_i("EverBlu requête compteur serial=%lu year=%u (%u octets)", serial, year, reqLen);

    // ---- Phase TX : wake-up + requête ----------------------
    _sendWakeupAndRequest(reqBuf, reqLen);
    log_i("TX terminé — attente ACK");

    // ---- Phase RX 1 : ACK (sync 0x55 0x50, 18 octets) -----
    if (!_receiveAck(250)) {
        log_w("Pas d'ACK reçu (compteur absent, hors fenêtre, ou mauvais serial/année ?)");
        _radio.idle();
        return false;
    }
    log_i("ACK reçu — attente données");

    // ---- Phase RX 2 : données (4× oversampled) -------------
    uint8_t rawBuf[300];
    uint8_t rawLen = 0;
    if (!_receiveData(rawBuf, rawLen, 800)) {
        log_w("Pas de données reçues après ACK");
        _radio.idle();
        return false;
    }
    log_i("Données brutes reçues : %u octets", rawLen);

    // ---- Décodage ------------------------------------------
    uint8_t decoded[64];
    uint8_t decodedLen = 0;
    if (!_decodeResponse(rawBuf, rawLen, decoded, decodedLen)) {
        log_w("Échec décodage réponse (%u octets bruts)", rawLen);
        _radio.idle();
        return false;
    }
    log_i("Décodé : %u octets", decodedLen);

    // ---- Extraction valeurs --------------------------------
    out.rssi = _radio.readRSSI();
    if (!_parseData(decoded, decodedLen, out)) {
        log_w("Payload trop court pour extraire les données (%u octets)", decodedLen);
        _radio.idle();
        return false;
    }

    _radio.idle();
    return true;
}

// ============================================================
//  Construction de la trame de requête
// ============================================================

uint8_t EverBlu::_buildRequest(uint32_t serial, uint8_t year, uint8_t* out)
{
    // Payload (17 octets de données + 2 octets CRC = 19 octets)
    uint8_t payload[19];
    memcpy(payload, REQ_TEMPLATE, sizeof(REQ_TEMPLATE));
    payload[16] = 0x40;   // dernier octet fixe du template

    // Insertion de l'année et du serial
    payload[4] = (uint8_t)year;
    payload[5] = (uint8_t)((serial >> 16) & 0xFF);  // MSB
    payload[6] = (uint8_t)((serial >>  8) & 0xFF);
    payload[7] = (uint8_t)( serial        & 0xFF);  // LSB

    // CRC Kermit sur les 17 premiers octets
    uint16_t crc = _crcKermit(payload, 17);
    payload[17] = (uint8_t)(crc >> 8);    // high byte (déjà swappé par _crcKermit)
    payload[18] = (uint8_t)(crc & 0xFF);  // low byte

    // Assemblage : préfixe sync (9 octets) + payload encodé série
    memcpy(out, SYNC_PREFIX, 9);
    uint8_t encLen = _encodeBytes(payload, 19, out + 9);

    return 9 + encLen;  // ≈ 38–39 octets
}

// ============================================================
//  TX : wake-up (~2s de 0x55) puis requête
// ============================================================

void EverBlu::_sendWakeupAndRequest(const uint8_t* reqBuf, uint8_t reqLen)
{
    static const uint8_t WUP[8] = {0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55};
    const uint8_t WUP_REPEATS = 94; // 94 × 8 octets ≈ 2.5 secondes à 2.4 kbps

    // Mode TX : longueur infinie, sans sync word (porteuse brute de 0x55)
    _radio.idle();
    _radio.writeReg(CC1101_MDMCFG2, 0x00);   // Pas de sync word
    _radio.writeReg(CC1101_PKTCTRL0, 0x02);  // Mode longueur infinie

    // Écrire les premiers 8 octets dans le FIFO et démarrer TX
    _radio.writeFifo(WUP, 8);
    _radio.strobe(CC1101_STX);

    // Refill du FIFO pendant le wake-up
    for (uint8_t rep = 1; rep < WUP_REPEATS; rep++) {
        // Attendre de la place dans le FIFO
        uint32_t t = millis();
        while (_radio.txFifoFree() < 8) {
            if (millis() - t > 3000) goto tx_done;
            delay(5);
        }
        _radio.writeFifo(WUP, 8);
    }

    // Pause 130 ms (le CC1101 continue d'émettre les derniers octets bufférisés)
    delay(130);

    // Envoyer la trame de requête dans le FIFO
    {
        uint32_t t = millis();
        while (_radio.txFifoFree() < reqLen) {
            if (millis() - t > 2000) goto tx_done;
            delay(5);
        }
        _radio.writeFifo(reqBuf, reqLen);
    }

    // Attendre la fin de la transmission.
    // En mode infini le CC1101 passe en TX_UNDERFLOW quand le FIFO se vide,
    // et TXBYTES n'est plus fiable dans cet état. On attend donc un temps
    // calculé : reqLen octets à 2.4 kbps ≈ reqLen × 3.33 ms, + marge.
    delay((uint32_t)reqLen * 4 + 20);

tx_done:
    // Forcer IDLE (nécessaire car en mode infini le CC1101 est en TX_UNDERFLOW)
    _radio.idle();
    // Rétablir la config normale (sync word actif, longueur fixe)
    _radio.writeReg(CC1101_MDMCFG2, 0x02);
    _radio.writeReg(CC1101_PKTCTRL0, 0x00);
}

// ============================================================
//  RX 1 : ACK (2.4 kbps, sync 0x5550, 18 octets)
// ============================================================

bool EverBlu::_receiveAck(uint32_t timeoutMs)
{
    // Configurer pour recevoir l'ACK
    _radio.idle();
    _radio.writeReg(CC1101_SYNC1,   0x55);
    _radio.writeReg(CC1101_SYNC0,   0x50);
    _radio.writeReg(CC1101_PKTLEN,  18);
    _radio.writeReg(CC1101_PKTCTRL0, 0x00); // Longueur fixe
    _radio.writeReg(CC1101_MDMCFG4, 0xF6);  // 2.4 kbps
    _radio.writeReg(CC1101_MDMCFG3, 0x83);
    _radio.strobe(CC1101_SRX);

    // Attendre 18 octets dans le FIFO
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        if (_radio.rxFifoBytes() >= 18) {
            uint8_t dummy[18];
            _radio.drainFifo(dummy, 18);
            return true;
        }
        delay(5);
    }
    return false;
}

// ============================================================
//  RX 2 : données (9.6 kbps = 4× oversampled, sync 0xFFF0)
// ============================================================

bool EverBlu::_receiveData(uint8_t* buf, uint8_t& len, uint32_t timeoutMs)
{
    len = 0;

    // Reconfigurer pour données 4× oversampled
    _radio.idle();
    _radio.writeReg(CC1101_SYNC1,    0xFF);
    _radio.writeReg(CC1101_SYNC0,    0xF0);
    _radio.writeReg(CC1101_MDMCFG4,  0xF8);  // DRATE_E=8 → 9.6 kbps
    _radio.writeReg(CC1101_MDMCFG3,  0x83);  // DRATE_M=131
    _radio.writeReg(CC1101_PKTCTRL0, 0x02);  // Mode infini
    _radio.strobe(CC1101_SRX);

    // Lire le FIFO jusqu'au timeout ou buffer plein
    uint32_t start    = millis();
    uint32_t lastRxMs = millis();
    bool     gotData  = false;

    while (millis() - start < timeoutMs && len < 255) {
        uint8_t avail = _radio.rxFifoBytes();
        if (avail > 0) {
            uint8_t toRead = ((uint16_t)len + avail > 255) ? (255 - len) : avail;
            _radio.drainFifo(buf + len, toRead);
            len     += toRead;
            lastRxMs = millis();
            gotData  = true;
        } else if (gotData && millis() - lastRxMs > 150) {
            break;  // Plus de données depuis 150 ms → fin du paquet
        }
        delay(2);
    }

    // Rétablir la config de base
    _radio.writeReg(CC1101_SYNC1,    0x55);
    _radio.writeReg(CC1101_SYNC0,    0x00);
    _radio.writeReg(CC1101_MDMCFG4,  0xF6);
    _radio.writeReg(CC1101_MDMCFG3,  0x83);
    _radio.writeReg(CC1101_PKTCTRL0, 0x00);

    return len > 0;
}

// ============================================================
//  Décodage 4× oversampled + encodage série start/stop
//
//  Le signal reçu à 9.6 kbps encode des données à 2.4 kbps :
//  chaque bit original apparaît comme 4 bits identiques.
//  De plus, chaque octet est encadré par 1 bit start (0) et
//  3 bits stop (111) → 12 bits par octet à 2.4 kbps
//                    → 48 bits (6 octets) dans le flux 9.6 kbps.
// ============================================================

bool EverBlu::_decodeResponse(const uint8_t* raw, uint8_t rawLen,
                               uint8_t* out, uint8_t& outLen)
{
    outLen = 0;
    uint16_t totalBits = (uint16_t)rawLen * 8;
    uint16_t pos = 0;

    while (pos + 48 <= totalBits) {
        // Chercher un bit start (4 bits à 0)
        if (_bit4x(raw, pos) != 0) {
            pos += 4;
            continue;
        }
        pos += 4;  // Consommer le bit start

        if (pos + 44 > totalBits) break;

        // Lire 8 bits de données LSB-first (chacun × 4 dans le flux)
        uint8_t byte = 0;
        for (int b = 0; b < 8; b++) {
            if (_bit4x(raw, pos)) byte |= (1u << b);
            pos += 4;
        }

        // Consommer les 3 bits stop (× 4)
        pos += 12;

        out[outLen++] = byte;
        if (outLen >= 64) break;
    }

    return outLen > 0;
}

// ============================================================
//  Extraction des valeurs du buffer décodé
//  Offsets issus du reverse-engineering (hallard/psykokwak)
// ============================================================

bool EverBlu::_parseData(const uint8_t* d, uint8_t len, EverBluData& out)
{
    if (len < 32) return false;

    // Volume en litres : uint32 little-endian aux octets 18–21
    out.liters = (uint32_t)d[18]
               | ((uint32_t)d[19] << 8)
               | ((uint32_t)d[20] << 16)
               | ((uint32_t)d[21] << 24);

    // Batterie : mois restants, octet 31
    out.battery   = d[31];

    // Compteur de lectures : octet 48 (si disponible)
    out.readCount = (len > 48) ? d[48] : 0;

    out.valid = true;
    return true;
}

// ============================================================
//  Encodage série : start(0) + 8 bits LSB-first + stop(111)
//  = 12 bits par octet, packés MSB-first dans out[]
// ============================================================

uint8_t EverBlu::_encodeBytes(const uint8_t* in, uint8_t inLen, uint8_t* out)
{
    uint32_t shift = 0;
    int      bits  = 0;
    uint8_t  outLen = 0;

    for (uint8_t i = 0; i < inLen; i++) {
        // Mot de 12 bits : bit11=start=0, bits10..3=données LSB-first, bits2..0=stop=111
        uint16_t word = 0x007;  // stop bits
        for (int b = 0; b < 8; b++) {
            if (in[i] & (1u << b)) word |= (1u << (10 - b));
        }
        // bit 11 = start = 0 (déjà à 0)

        shift = (shift << 12) | word;
        bits += 12;

        while (bits >= 8) {
            bits -= 8;
            out[outLen++] = (uint8_t)((shift >> bits) & 0xFF);
        }
    }

    if (bits > 0) {
        out[outLen++] = (uint8_t)((shift << (8 - bits)) & 0xFF);
    }

    return outLen;
}

// ============================================================
//  CRC Kermit (polynôme 0x8408, init 0x0000)
//  Les bytes du résultat sont swappés avant retour.
// ============================================================

uint16_t EverBlu::_crcKermit(const uint8_t* data, uint8_t len)
{
    uint16_t crc = 0x0000;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++) {
            if (crc & 1) crc = (crc >> 1) ^ 0x8408;
            else         crc >>= 1;
        }
    }
    // Swap high/low bytes
    return ((crc & 0xFF) << 8) | (crc >> 8);
}

// ============================================================
//  Helpers bits
// ============================================================

uint8_t EverBlu::_bit(const uint8_t* buf, uint16_t pos)
{
    return (buf[pos / 8] >> (7 - pos % 8)) & 1;
}

uint8_t EverBlu::_bit4x(const uint8_t* buf, uint16_t pos)
{
    // Vote majoritaire sur 4 bits consécutifs
    uint8_t ones = _bit(buf, pos) + _bit(buf, pos+1)
                 + _bit(buf, pos+2) + _bit(buf, pos+3);
    return (ones >= 2) ? 1 : 0;
}
