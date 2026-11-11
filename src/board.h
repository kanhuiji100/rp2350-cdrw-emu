/*
 * Board wiring for the reference build.  All pins can be overridden from the
 * compiler command line (for example -DCDRW_SD_SCK=18) if you use a different
 * RP2350 board or need to move the microSD wiring.
 */
#ifndef CDRW_BOARD_H
#define CDRW_BOARD_H

#include "hardware/spi.h"
#include "hardware/i2c.h"

/* ---- microSD (SPI0) -------------------------------------------------- */
#ifndef CDRW_SD_SPI
#define CDRW_SD_SPI spi0
#endif
#ifndef CDRW_SD_SCK
#define CDRW_SD_SCK 2
#endif
#ifndef CDRW_SD_MOSI
#define CDRW_SD_MOSI 3
#endif
#ifndef CDRW_SD_MISO
#define CDRW_SD_MISO 4
#endif
#ifndef CDRW_SD_CS
#define CDRW_SD_CS 5
#endif
/* Card-detect switch, active low to GND.  Set to -1 when not wired. */
#ifndef CDRW_SD_CD
#define CDRW_SD_CD -1
#endif
/* SPI clock.  Most cards work at 25 MHz; use 12.5 MHz for long wires. */
#ifndef CDRW_SD_SPI_HZ
#define CDRW_SD_SPI_HZ (12 * 1000 * 1000)
#endif

#ifndef CDRW_SD_READ_HZ
#define CDRW_SD_READ_HZ (12 * 1000 * 1000)
#endif

#ifndef CDRW_SD_WRITE_HZ
#define CDRW_SD_WRITE_HZ (CDRW_SD_SPI_HZ)
#endif

/* ---- SSD1306 128x64 OLED (I2C1) ------------------------------------- */
#ifndef CDRW_OLED_I2C
#define CDRW_OLED_I2C i2c1
#endif
#ifndef CDRW_OLED_SDA
#define CDRW_OLED_SDA 6
#endif
#ifndef CDRW_OLED_SCL
#define CDRW_OLED_SCL 7
#endif
#ifndef CDRW_OLED_ADDR
#define CDRW_OLED_ADDR 0x3C
#endif
#ifndef CDRW_OLED_I2C_HZ
#define CDRW_OLED_I2C_HZ (400 * 1000)
#endif

/* ---- Rotary encoder + push button ---------------------------------- */
#ifndef CDRW_ENC_A
#define CDRW_ENC_A 8
#endif
#ifndef CDRW_ENC_B
#define CDRW_ENC_B 9
#endif
#ifndef CDRW_ENC_BTN
#define CDRW_ENC_BTN 10
#endif

/* ---- Status LED ----------------------------------------------------- */
#ifndef CDRW_LED_PIN
#ifdef PICO_DEFAULT_LED_PIN
#define CDRW_LED_PIN PICO_DEFAULT_LED_PIN
#else
#define CDRW_LED_PIN 25
#endif
#endif
#ifndef CDRW_LED_ACTIVE_HIGH
#define CDRW_LED_ACTIVE_HIGH 1
#endif

#endif /* CDRW_BOARD_H */
