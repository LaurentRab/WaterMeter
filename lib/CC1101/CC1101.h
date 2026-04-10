#pragma once
#include <Arduino.h>
#include <SPI.h>

// ============================================================
//  Driver CC1101 bas niveau — Itron EverBlu Cyble Enhanced
//  433.82 MHz · 2-FSK · 2.4 kbps
// ============================================================

// ---- Registres de configuration ----------------------------
#define CC1101_IOCFG2    0x00
#define CC1101_IOCFG1    0x01
#define CC1101_IOCFG0    0x02
#define CC1101_FIFOTHR   0x03
#define CC1101_SYNC1     0x04
#define CC1101_SYNC0     0x05
#define CC1101_PKTLEN    0x06
#define CC1101_PKTCTRL1  0x07
#define CC1101_PKTCTRL0  0x08
#define CC1101_FSCTRL1   0x0B
#define CC1101_FREQ2     0x0D
#define CC1101_FREQ1     0x0E
#define CC1101_FREQ0     0x0F
#define CC1101_MDMCFG4   0x10
#define CC1101_MDMCFG3   0x11
#define CC1101_MDMCFG2   0x12
#define CC1101_MDMCFG1   0x13
#define CC1101_MDMCFG0   0x14
#define CC1101_DEVIATN   0x15
#define CC1101_MCSM1     0x17
#define CC1101_MCSM0     0x18
#define CC1101_FOCCFG    0x19
#define CC1101_BSCFG     0x1A
#define CC1101_AGCCTRL2  0x1B
#define CC1101_AGCCTRL1  0x1C
#define CC1101_AGCCTRL0  0x1D
#define CC1101_WORCTRL   0x20
#define CC1101_FREND1    0x21
#define CC1101_FREND0    0x22
#define CC1101_FSCAL3    0x23
#define CC1101_FSCAL2    0x24
#define CC1101_FSCAL1    0x25
#define CC1101_FSCAL0    0x26
#define CC1101_TEST2     0x2C
#define CC1101_TEST1     0x2D
#define CC1101_TEST0     0x2E

// ---- Registres de statut (lire avec READ|BURST) ------------
#define CC1101_RSSI      0x34
#define CC1101_MARCSTATE 0x35
#define CC1101_TXBYTES   0x3A
#define CC1101_RXBYTES   0x3B

// ---- Strobes -----------------------------------------------
#define CC1101_SRES      0x30
#define CC1101_SRX       0x34
#define CC1101_STX       0x35
#define CC1101_SIDLE     0x36
#define CC1101_SFRX      0x3A
#define CC1101_SFTX      0x3B
#define CC1101_SNOP      0x3D

// ---- FIFO --------------------------------------------------
#define CC1101_RXFIFO    0x3F
#define CC1101_TXFIFO    0x3F

// ---- Flags SPI ---------------------------------------------
#define CC1101_READ_FLAG  0x80
#define CC1101_BURST_FLAG 0x40

// ---- MARCSTATE values --------------------------------------
#define CC1101_STATE_IDLE  0x01
#define CC1101_STATE_RX    0x0D
#define CC1101_STATE_TX    0x13

class CC1101 {
public:
    CC1101(uint8_t csn, uint8_t gdo0, int8_t sck=-1, int8_t mosi=-1, int8_t miso=-1);

    // Initialise SPI + reset + vérifie version
    bool begin();

    // Configure pour EverBlu (2-FSK 2.4 kbps 433.82 MHz)
    void configureEverBlu();

    // Change la fréquence porteuse à chaud (sans reconfigurer les autres registres)
    void setFrequency(float mhz);

    // Passe en IDLE et vide les FIFOs
    void idle();

    // Test matériel complet : SPI loopback, FIFO, TX/RX transitions
    // Retourne true si tout est OK.
    bool selfTest();

    // ---- Interface bas niveau pour EverBlu ----------------
    void    writeReg(uint8_t addr, uint8_t val);
    uint8_t readReg(uint8_t addr);             // lit registre de config (single byte)
    uint8_t readStatus(uint8_t addr);          // lit registre de statut (BURST)
    void    writeFifo(const uint8_t* data, uint8_t len);   // burst write TX FIFO
    uint8_t drainFifo(uint8_t* buf, uint8_t maxLen);       // burst read RX FIFO
    void    strobe(uint8_t cmd);

    uint8_t txFifoFree();    // octets libres dans TX FIFO (64 - occupés)
    uint8_t rxFifoBytes();   // octets disponibles dans RX FIFO
    uint8_t marcstate();     // état courant (CC1101_STATE_*)

    int8_t  readRSSI();

private:
    uint8_t _csn, _gdo0;
    int8_t  _sck, _mosi, _miso;

    void    _select();
    void    _deselect();
    void    _waitMiso();
    uint8_t _readReg(uint8_t addr);
    void    _writeReg(uint8_t addr, uint8_t val);
    void    _writeBurst(uint8_t addr, const uint8_t* data, uint8_t len);
    void    _readBurst(uint8_t addr, uint8_t* buf, uint8_t len);
    void    _strobe(uint8_t cmd);
    int8_t  _rssiRaw2dBm(uint8_t raw);
};
