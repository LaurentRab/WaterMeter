#include <Arduino.h>
#include "CC1101.h"

CC1101::CC1101(uint8_t csn, uint8_t gdo0, int8_t sck, int8_t mosi, int8_t miso)
    : _csn(csn), _gdo0(gdo0), _sck(sck), _mosi(mosi), _miso(miso)
{}

// ============================================================
//  Initialisation
// ============================================================

bool CC1101::begin()
{
    if (_sck >= 0) SPI.begin(_sck, _miso, _mosi, _csn);
    else           SPI.begin();

    SPI.setDataMode(SPI_MODE0);
    SPI.setBitOrder(MSBFIRST);
    SPI.setFrequency(500000);   // 500 kHz recommandé pour EverBlu

    pinMode(_csn,  OUTPUT);
    pinMode(_gdo0, INPUT);
    digitalWrite(_csn, HIGH);

    // Reset
    _select(); _waitMiso(); _strobe(CC1101_SRES); _waitMiso(); _deselect();
    delay(10);

    uint8_t version = _readReg(0x31 | CC1101_BURST_FLAG);
    if (version != 0x14)
        log_w("CC1101 version inattendue : 0x%02X (clone ?)", version);
    else
        log_i("CC1101 détecté OK (version 0x14)");

    return true;
}

// ============================================================
//  Configuration EverBlu Cyble Enhanced
//  Source : reverse-engineering hallard/psykokwak
//  2-FSK · 2.4 kbps · 433.82 MHz · déviation 5.157 kHz
// ============================================================

void CC1101::configureEverBlu()
{
    _writeReg(CC1101_IOCFG2,   0x0D);  // GDO2 : serial data output
    _writeReg(CC1101_IOCFG0,   0x06);  // GDO0 : assert sur sync word reçu/envoyé

    _writeReg(CC1101_FIFOTHR,  0x47);  // RX thr 33B / TX thr 32B

    _writeReg(CC1101_SYNC1,    0x55);  // Sync word : 0x55 0x00 (défaut, modifié par EverBlu)
    _writeReg(CC1101_SYNC0,    0x00);

    _writeReg(CC1101_PKTCTRL1, 0x00);  // Pas de vérif adresse
    _writeReg(CC1101_PKTCTRL0, 0x00);  // Longueur fixe, pas de CRC HW

    _writeReg(CC1101_FSCTRL1,  0x08);  // Fréquence intermédiaire

    // Fréquence : 433.82 MHz  (f = 26 MHz × FREQ / 2^16)
    // 0x10AF75 = 1093493 → 1093493 × 26e6 / 65536 = 433.820 MHz
    _writeReg(CC1101_FREQ2,    0x10);
    _writeReg(CC1101_FREQ1,    0xAF);
    _writeReg(CC1101_FREQ0,    0x75);

    // Modem : BW 58 kHz, 2.4 kbps, 2-FSK
    _writeReg(CC1101_MDMCFG4,  0xF6);  // CHANBW_E=3, CHANBW_M=3, DRATE_E=6 → BW=58kHz
    _writeReg(CC1101_MDMCFG3,  0x83);  // DRATE_M=131 → 2399 bps ≈ 2.4 kbps
    _writeReg(CC1101_MDMCFG2,  0x02);  // 2-FSK, pas de Manchester, 16/16 sync bits
    _writeReg(CC1101_MDMCFG1,  0x00);  // 2 octets de préambule
    _writeReg(CC1101_MDMCFG0,  0x00);  // Channel spacing 25 kHz

    // Déviation 2-FSK : 5.157 kHz
    _writeReg(CC1101_DEVIATN,  0x15);  // DEVIATN_E=1, DEVIATN_M=5

    // Machine d'état : retour IDLE après TX/RX (RXOFF=IDLE, TXOFF=IDLE)
    _writeReg(CC1101_MCSM1,    0x00);
    _writeReg(CC1101_MCSM0,    0x18);  // Auto-calibration IDLE→RX/TX

    _writeReg(CC1101_FOCCFG,   0x1D);
    _writeReg(CC1101_BSCFG,    0x1C);
    _writeReg(CC1101_AGCCTRL2, 0xC7);
    _writeReg(CC1101_AGCCTRL1, 0x00);
    _writeReg(CC1101_AGCCTRL0, 0xB2);

    _writeReg(CC1101_WORCTRL,  0xFB);

    _writeReg(CC1101_FREND1,   0xB6);
    _writeReg(CC1101_FREND0,   0x10);

    _writeReg(CC1101_FSCAL3,   0xE9);
    _writeReg(CC1101_FSCAL2,   0x2A);
    _writeReg(CC1101_FSCAL1,   0x00);
    _writeReg(CC1101_FSCAL0,   0x1F);

    // Requis pour les faibles débits (< 26 kbps)
    _writeReg(CC1101_TEST2,    0x81);
    _writeReg(CC1101_TEST1,    0x35);
    _writeReg(CC1101_TEST0,    0x09);

    // PA table : +10 dBm (~10 mW, puissance max CC1101)
    _select(); _waitMiso();
    SPI.transfer(0x7E | CC1101_BURST_FLAG);   // PATABLE burst write
    SPI.transfer(0xC0);  // +10 dBm max power (requis pour réveiller le compteur)
    for (int i = 1; i < 8; i++) SPI.transfer(0x00);
    _deselect();

    log_i("CC1101 configuré : 433.82 MHz · 2-FSK · 2.4 kbps (EverBlu)");
}

// ============================================================
//  Changement de fréquence à chaud
// ============================================================

void CC1101::setFrequency(float mhz)
{
    // FREQ = f_Hz / f_XOSC * 2^16  (f_XOSC = 26 MHz)
    uint32_t reg = (uint32_t)(mhz * 1e6f / 26e6f * 65536.0f + 0.5f);
    _writeReg(CC1101_FREQ2, (reg >> 16) & 0xFF);
    _writeReg(CC1101_FREQ1, (reg >> 8)  & 0xFF);
    _writeReg(CC1101_FREQ0,  reg        & 0xFF);
    log_i("CC1101 fréquence : %.3f MHz (reg=0x%06lX)", mhz, reg);
}

// ============================================================
//  IDLE
// ============================================================

void CC1101::idle()
{
    _strobe(CC1101_SIDLE);
    delay(2);
    _strobe(CC1101_SFRX);
    _strobe(CC1101_SFTX);
}

// ============================================================
//  Interface bas niveau publique (pour EverBlu)
// ============================================================

void CC1101::writeReg(uint8_t addr, uint8_t val)    { _writeReg(addr, val); }
void CC1101::strobe(uint8_t cmd)                     { _strobe(cmd); }

uint8_t CC1101::readStatus(uint8_t addr)
{
    return _readReg(addr | CC1101_BURST_FLAG);
}

void CC1101::writeFifo(const uint8_t* data, uint8_t len)
{
    _writeBurst(CC1101_TXFIFO, data, len);
}

uint8_t CC1101::drainFifo(uint8_t* buf, uint8_t maxLen)
{
    uint8_t avail = readStatus(CC1101_RXBYTES) & 0x7F;
    if (avail == 0) return 0;
    uint8_t n = (avail < maxLen) ? avail : maxLen;
    _readBurst(CC1101_RXFIFO, buf, n);
    return n;
}

uint8_t CC1101::txFifoFree()
{
    uint8_t used = readStatus(CC1101_TXBYTES) & 0x7F;
    return (used >= 64) ? 0 : (64 - used);
}

uint8_t CC1101::rxFifoBytes()
{
    return readStatus(CC1101_RXBYTES) & 0x7F;
}

uint8_t CC1101::marcstate()
{
    return readStatus(CC1101_MARCSTATE) & 0x1F;
}

int8_t CC1101::readRSSI()
{
    return _rssiRaw2dBm(readStatus(CC1101_RSSI));
}

// ============================================================
//  SPI bas niveau
// ============================================================

void CC1101::_select()   { digitalWrite(_csn, LOW); }
void CC1101::_deselect() { digitalWrite(_csn, HIGH); }

void CC1101::_waitMiso()
{
    uint32_t t = millis() + 100;
    while (digitalRead(_miso >= 0 ? _miso : MISO) && millis() < t);
}

uint8_t CC1101::_readReg(uint8_t addr)
{
    _select(); _waitMiso();
    SPI.transfer(addr | CC1101_READ_FLAG);
    uint8_t v = SPI.transfer(0x00);
    _deselect();
    return v;
}

void CC1101::_writeReg(uint8_t addr, uint8_t val)
{
    _select(); _waitMiso();
    SPI.transfer(addr);
    SPI.transfer(val);
    _deselect();
}

void CC1101::_writeBurst(uint8_t addr, const uint8_t* data, uint8_t len)
{
    _select(); _waitMiso();
    SPI.transfer(addr | CC1101_BURST_FLAG);
    for (uint8_t i = 0; i < len; i++) SPI.transfer(data[i]);
    _deselect();
}

void CC1101::_readBurst(uint8_t addr, uint8_t* buf, uint8_t len)
{
    _select(); _waitMiso();
    SPI.transfer(addr | CC1101_READ_FLAG | CC1101_BURST_FLAG);
    for (uint8_t i = 0; i < len; i++) buf[i] = SPI.transfer(0x00);
    _deselect();
}

void CC1101::_strobe(uint8_t cmd)
{
    _select(); _waitMiso();
    SPI.transfer(cmd);
    _deselect();
}

int8_t CC1101::_rssiRaw2dBm(uint8_t raw)
{
    if (raw >= 128) return (int8_t)((raw - 256) / 2) - 74;
    return (int8_t)(raw / 2) - 74;
}
