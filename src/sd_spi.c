/*
 * Minimal, robust SPI-mode SD/SDHC/SDXC driver.
 *
 * Deliberately single-card, single-threaded, block-addressed at 512 bytes.
 * Multi-block transfers are used for speed; every operation has a timeout so
 * a missing card or a dirty contact cannot hang the USB stack forever.
 */
#include "sd_spi.h"

#include <string.h>

#include "board.h"
#include "debug_log.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "hardware/spi.h"
#include "pico/time.h"

#define CMD0 0
#define CMD8 8
#define CMD9 9
#define CMD12 12
#define CMD16 16
#define CMD17 17
#define CMD18 18
#define CMD23 23
#define CMD24 24
#define CMD25 25
#define CMD41 41
#define CMD55 55
#define CMD58 58

#define R1_IDLE 0x01
#define R1_ILLEGAL 0x04
#define DATA_TOKEN_START 0xFE
#define DATA_TOKEN_START_MULTI 0xFC
#define DATA_TOKEN_STOP 0xFD
#define DATA_RESP_ACCEPTED 0x05

static bool wait_not_busy(uint32_t timeout_ms);

static bool s_ready;
static bool s_sdhc;
static uint32_t s_blocks;
static int8_t s_cmd25_state; /* 0=unknown, 1=works, -1=unsupported */
static uint32_t s_write_hz = CDRW_SD_WRITE_HZ;
static uint32_t s_read_hz = CDRW_SD_READ_HZ;
static bool s_write_hz_logged;
static bool s_read_hz_logged;

static inline void cs_low(void)
{
    gpio_put(CDRW_SD_CS, 0);
    spi_write_blocking(CDRW_SD_SPI, (const uint8_t[]){0xFF}, 1);
}

static inline void cs_high(void)
{
    gpio_put(CDRW_SD_CS, 1);
    spi_write_blocking(CDRW_SD_SPI, (const uint8_t[]){0xFF}, 1);
}

static int s_dma_tx = -1;
static int s_dma_rx = -1;
static uint8_t s_dma_ff[SD_BLOCK_SIZE];
static uint8_t s_dma_dummy[SD_BLOCK_SIZE];

static void sd_dma_init(void)
{
    if (s_dma_tx >= 0) {
        return;
    }
    s_dma_tx = dma_claim_unused_channel(true);
    s_dma_rx = dma_claim_unused_channel(true);
    memset(s_dma_ff, 0xFF, sizeof(s_dma_ff));
}

static void spi_dma_transfer(const void *tx, void *rx, size_t len)
{
    if (s_dma_tx < 0 || s_dma_rx < 0) {
        if (rx) {
            spi_write_read_blocking(CDRW_SD_SPI, (const uint8_t *)tx, (uint8_t *)rx, len);
        } else {
            spi_write_blocking(CDRW_SD_SPI, (const uint8_t *)tx, len);
        }
        return;
    }

    volatile void *dr = (volatile void *)&spi_get_hw(CDRW_SD_SPI)->dr;

    dma_channel_config tx_c = dma_channel_get_default_config(s_dma_tx);
    channel_config_set_transfer_data_size(&tx_c, DMA_SIZE_8);
    channel_config_set_read_increment(&tx_c, true);
    channel_config_set_write_increment(&tx_c, false);
    channel_config_set_dreq(&tx_c, spi_get_dreq(CDRW_SD_SPI, true));
    dma_channel_configure(s_dma_tx, &tx_c, dr, tx, len, false);

    dma_channel_config rx_c = dma_channel_get_default_config(s_dma_rx);
    channel_config_set_transfer_data_size(&rx_c, DMA_SIZE_8);
    channel_config_set_read_increment(&rx_c, false);
    channel_config_set_write_increment(&rx_c, true);
    channel_config_set_dreq(&rx_c, spi_get_dreq(CDRW_SD_SPI, false));
    dma_channel_configure(s_dma_rx, &rx_c, rx ? rx : s_dma_dummy, dr, len, false);

    dma_channel_start(s_dma_rx);
    dma_channel_start(s_dma_tx);
    dma_channel_wait_for_finish_blocking(s_dma_tx);
    dma_channel_wait_for_finish_blocking(s_dma_rx);
}

static void spi_read_dma(uint8_t *dst, size_t len)
{
    spi_dma_transfer(s_dma_ff, dst, len);
}

static void spi_write_dma(const uint8_t *src, size_t len)
{
    spi_dma_transfer(src, NULL, len);
}

static inline uint8_t xfer(uint8_t value)
{
    uint8_t rx;
    spi_write_read_blocking(CDRW_SD_SPI, &value, &rx, 1);
    return rx;
}

static uint8_t crc7(const uint8_t *data, int len)
{
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        uint8_t d = data[i];
        for (int b = 0; b < 8; b++) {
            crc <<= 1;
            if ((d ^ crc) & 0x80) {
                crc ^= 0x09;
            }
            d <<= 1;
        }
    }
    return (uint8_t)((crc << 1) | 1);
}

static uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static uint8_t wait_r1(uint32_t timeout_us)
{
    uint32_t start = time_us_32();
    for (;;) {
        uint8_t r = xfer(0xFF);
        if ((r & 0x80) == 0) {
            return r;
        }
        if ((uint32_t)(time_us_32() - start) > timeout_us) {
            return 0xFF;
        }
    }
}

/* Send command, return R1.  Keeps CS asserted. */
static uint8_t cmd_begin(uint8_t cmd, uint32_t arg, uint8_t crc, bool keep_selected)
{
    uint8_t frame[6];
    frame[0] = (uint8_t)(0x40 | cmd);
    frame[1] = (uint8_t)(arg >> 24);
    frame[2] = (uint8_t)(arg >> 16);
    frame[3] = (uint8_t)(arg >> 8);
    frame[4] = (uint8_t)arg;
    frame[5] = crc ? crc : crc7(frame, 5);

    cs_low();
    spi_write_blocking(CDRW_SD_SPI, frame, sizeof(frame));
    uint8_t r = wait_r1(100000); /* 100 ms */
    if (!keep_selected) {
        cs_high();
    }
    return r;
}

/* Send command and read extra response bytes while CS stays asserted. */
static uint8_t cmd_xfer(uint8_t cmd, uint32_t arg, uint8_t crc,
                        uint8_t *extra, uint32_t extra_len)
{
    uint8_t r = cmd_begin(cmd, arg, crc, true);
    for (uint32_t i = 0; i < extra_len; i++) {
        extra[i] = xfer(0xFF);
    }
    cs_high();
    return r;
}

bool sd_card_present(void)
{
#if CDRW_SD_CD >= 0
    return gpio_get(CDRW_SD_CD) == 0;
#else
    return true;
#endif
}

static bool read_register(uint8_t cmd, uint8_t out[16])
{
    uint8_t r = cmd_begin(cmd, 0, 0, true);
    if (r != 0) {
        cs_high();
        return false;
    }
    uint32_t start = time_us_32();
    uint8_t token = 0xFF;
    do {
        token = xfer(0xFF);
    } while (token == 0xFF && (uint32_t)(time_us_32() - start) < 100000u);
    if (token != DATA_TOKEN_START) {
        cs_high();
        return false;
    }
    spi_read_blocking(CDRW_SD_SPI, 0xFF, out, 16);
    (void)xfer(0xFF);
    (void)xfer(0xFF);
    cs_high();
    return true;
}

static uint32_t csd_capacity_blocks(const uint8_t csd[16])
{
    uint8_t csd_structure = csd[0] >> 6;
    if (csd_structure == 1) {
        uint32_t c_size = ((uint32_t)(csd[7] & 0x3F) << 16) |
                          ((uint32_t)csd[8] << 8) | csd[9];
        /* (C_SIZE + 1) * 1024 * 512-byte blocks */
        return (c_size + 1u) * 1024u;
    }
    if (csd_structure == 0) {
        uint32_t c_size = ((uint32_t)(csd[6] & 0x03) << 10) |
                          ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
        uint32_t c_size_mult = ((csd[9] & 0x03) << 1) | (csd[10] >> 7);
        uint32_t read_bl_len = csd[5] & 0x0F;
        uint32_t block_len = 1u << read_bl_len;
        if (block_len < 512u) {
            return 0;
        }
        uint64_t blocks = ((uint64_t)(c_size + 1) * (1u << (c_size_mult + 2))) *
                          (block_len / 512u);
        return (uint32_t)blocks;
    }
    return 0;
}

bool sd_init(void)
{
    s_ready = false;
    s_sdhc = false;
    s_blocks = 0;
    s_cmd25_state = 0;

    spi_init(CDRW_SD_SPI, 400 * 1000);
    spi_set_format(CDRW_SD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    sd_dma_init();
    gpio_set_function(CDRW_SD_SCK, GPIO_FUNC_SPI);
    gpio_set_function(CDRW_SD_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(CDRW_SD_MISO, GPIO_FUNC_SPI);
    gpio_pull_up(CDRW_SD_MISO);
    gpio_init(CDRW_SD_CS);
    gpio_set_dir(CDRW_SD_CS, GPIO_OUT);
    gpio_put(CDRW_SD_CS, 1);

#if CDRW_SD_CD >= 0
    gpio_init(CDRW_SD_CD);
    gpio_set_dir(CDRW_SD_CD, GPIO_IN);
    gpio_pull_up(CDRW_SD_CD);
#endif

    /* At least 74 clocks with CS high. */
    uint8_t clocks[10] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    spi_write_blocking(CDRW_SD_SPI, clocks, sizeof(clocks));
    sleep_ms(2);

    if (!sd_card_present()) {
        LOGF("SD: card detect says no card");
        return false;
    }

    /* CMD0: reset to idle.  After a failed high-speed read the card may
       still be in data mode, so deselect it, clock it and retry. */
    bool v2 = false;
    uint8_t r = 0xFF;
    for (int attempt = 0; attempt < 3; attempt++) {
        cs_high();
        uint8_t ones[16];
        memset(ones, 0xFF, sizeof(ones));
        spi_write_blocking(CDRW_SD_SPI, ones, sizeof(ones));
        if (attempt) {
            sleep_ms(50);
        }
        r = cmd_begin(CMD0, 0, 0x95, false);
        if (r == R1_IDLE) {
            break;
        }
        LOGF("SD: CMD0 retry %d failed (R1=%02X)", attempt + 1, r);
    }
    if (r != R1_IDLE) {
        LOGF("SD: CMD0 failed (R1=%02X)", r);
        return false;
    }
    LOGF("SD: CMD0 idle OK");

    /* CMD8: check 2.7-3.6 V and SDHC support. */
    uint8_t r7[4] = {0};
    r = cmd_xfer(CMD8, 0x1AA, 0x87, r7, sizeof(r7));
    if (r == R1_IDLE) {
        v2 = true;
        if (r7[2] != 0x01 || r7[3] != 0xAA) {
            LOGF("SD: CMD8 voltage echo mismatch");
            return false;
        }
    } else if (r & R1_ILLEGAL) {
        v2 = false;
        LOGF("SD: CMD8 illegal, assuming SD v1 (SDSC)");
    } else {
        LOGF("SD: CMD8 failed (R1=%02X)", r);
        return false;
    }

    /* ACMD41 until the card leaves idle. */
    uint32_t start = time_us_32();
    bool ready = false;
    while ((uint32_t)(time_us_32() - start) < 2000000u) {
        r = cmd_begin(CMD55, 0, 0, false);
        if (r > R1_IDLE) {
            LOGF("SD: ACMD41 CMD55 failed (R1=%02X)", r);
            return false;
        }
        r = cmd_begin(CMD41, v2 ? 0x40000000u : 0, 0, false);
        if (r == 0) {
            ready = true;
            break;
        }
        if (r > R1_IDLE) {
            LOGF("SD: ACMD41 failed (R1=%02X)", r);
            return false;
        }
        sleep_ms(2);
    }
    if (!ready) {
        LOGF("SD: ACMD41 timeout");
        return false;
    }

    /* CMD58: read OCR, check power-up status and CCS.
       CMD58 is an SD v2 command.  Old SDSC (SD v1.x) cards, including many
       2 GB cards, reject it with ILLEGAL COMMAND.  Skipping it for v1 is
       essential; otherwise a perfectly good card fails initialization. */
    s_sdhc = false;
    if (v2) {
        uint8_t ocr[4] = {0};
        r = cmd_xfer(CMD58, 0, 0, ocr, sizeof(ocr));
        if (r != 0 || !(ocr[0] & 0x80)) {
            LOGF("SD: CMD58 failed (R1=%02X OCR=%02X)", r, ocr[0]);
            return false;
        }
        s_sdhc = (ocr[0] & 0x40) != 0;
        if (!s_sdhc) {
            /* SD v2 SDSC: byte addressing, capacity comes from CSD. */
            s_sdhc = false;
        }
    }

    /* SDHC/SDXC use fixed 512-byte blocks and may reject CMD16.  Only SDSC
       needs an explicit block length. */
    if (!s_sdhc) {
        r = cmd_begin(CMD16, 512, 0, false);
        if (r != 0) {
            LOGF("SD: CMD16 failed (R1=%02X)", r);
            return false;
        }
    }

    uint8_t csd[16];
    if (read_register(CMD9, csd)) {
        s_blocks = csd_capacity_blocks(csd);
    }

    spi_set_baudrate(CDRW_SD_SPI, CDRW_SD_SPI_HZ);
    s_ready = true;
    LOGF("SD: init OK, type=%s, blocks=%lu, SPI=%lu Hz (actual %lu Hz)",
         s_sdhc ? "SDHC/SDXC" : "SDSC", (unsigned long)s_blocks,
         (unsigned long)CDRW_SD_SPI_HZ,
         (unsigned long)spi_get_baudrate(CDRW_SD_SPI));
    return true;
}

bool sd_is_ready(void)
{
    return s_ready;
}

uint32_t sd_block_count(void)
{
    return s_blocks;
}

static uint32_t sd_addr(uint32_t lba)
{
    return s_sdhc ? lba : (lba * SD_BLOCK_SIZE);
}

void sd_fallback_read_clock(void)
{
    if (s_read_hz != CDRW_SD_SPI_HZ) {
        s_read_hz = CDRW_SD_SPI_HZ;
        LOGF("SD: read clock fallback to %lu Hz", (unsigned long)s_read_hz);
    }
}

bool sd_read_blocks(uint32_t lba, void *dst, uint32_t count)
{
    uint32_t t0 = time_us_32();
    if (!s_ready || !dst || !count || !sd_card_present()) {
        return false;
    }
    if (s_blocks && (lba + count > s_blocks)) {
        return false;
    }
    uint8_t *out = (uint8_t *)dst;
    uint32_t addr = sd_addr(lba);
    spi_set_baudrate(CDRW_SD_SPI, s_read_hz);
    if (!s_read_hz_logged) {
        s_read_hz_logged = true;
        LOGF("SD: read clock=%lu Hz", (unsigned long)spi_get_baudrate(CDRW_SD_SPI));
    }

    if (count == 1) {
        uint8_t r = cmd_begin(CMD17, addr, 0, true);
        if (r != 0) {
            cs_high();
            return false;
        }
        uint32_t start = time_us_32();
        uint8_t token;
        do {
            token = xfer(0xFF);
            if ((uint32_t)(time_us_32() - start) > 200000u) {
                cs_high();
                return false;
            }
        } while (token == 0xFF);
        if (token != DATA_TOKEN_START) {
            cs_high();
            return false;
        }
        spi_read_dma(out, SD_BLOCK_SIZE);
        (void)xfer(0xFF);
        (void)xfer(0xFF);
        cs_high();
        if (CDRW_READ_TRACE) {
            LOGF("SD read lba=%lu n=1 dt_us=%lu",
                 (unsigned long)lba, (unsigned long)(time_us_32() - t0));
        }
        return true;
    }

    uint8_t r = cmd_begin(CMD18, addr, 0, true);
    if (r != 0) {
        cs_high();
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t start = time_us_32();
        uint8_t token;
        do {
            token = xfer(0xFF);
            if ((uint32_t)(time_us_32() - start) > 200000u) {
                (void)xfer(0xFF);
                cs_high();
                return false;
            }
        } while (token == 0xFF);
        if (token != DATA_TOKEN_START) {
            cs_high();
            return false;
        }
        spi_read_dma(out + i * SD_BLOCK_SIZE, SD_BLOCK_SIZE);
        (void)xfer(0xFF);
        (void)xfer(0xFF);
    }
    /* The card is still busy for a short time after the final data block.
       CMD12 must not be sent until MISO goes high, otherwise wait_r1() in
       cmd_begin() times out after 100 ms on every multi-block read. */
    if (!wait_not_busy(20)) {
        LOGF("SD: read busy timeout before CMD12 lba=%lu n=%lu",
             (unsigned long)lba, (unsigned long)count);
        cs_high();
        return false;
    }
    uint8_t stop_r = cmd_begin(CMD12, 0, 0, true);
    if (stop_r != 0xFF) {
        (void)xfer(0xFF);
    }
    uint32_t start = time_us_32();
    while (xfer(0xFF) != 0xFF && (uint32_t)(time_us_32() - start) < 5000u) {
        tight_loop_contents();
    }
    cs_high();
    if (CDRW_READ_TRACE) {
        LOGF("SD read lba=%lu n=%lu dt_us=%lu",
             (unsigned long)lba, (unsigned long)count,
             (unsigned long)(time_us_32() - t0));
    }
    return true;
}

static bool wait_not_busy(uint32_t timeout_ms)
{
    uint32_t start = time_us_32();
    for (;;) {
        if (xfer(0xFF) == 0xFF) {
            return true;
        }
        if ((uint32_t)(time_us_32() - start) > timeout_ms * 1000u) {
            return false;
        }
        tight_loop_contents();
    }
}


static void sd_abort_multi_write(void)
{
    uint8_t stop = DATA_TOKEN_STOP;
    spi_write_blocking(CDRW_SD_SPI, &stop, 1);
    (void)xfer(0xFF);
    sleep_us(500);
    cs_high();
    uint8_t ones[16];
    memset(ones, 0xFF, sizeof(ones));
    spi_write_blocking(CDRW_SD_SPI, ones, sizeof(ones));
    sleep_ms(10);
}

static bool sd_write_multi(uint32_t lba, const uint8_t *in, uint32_t count)
{
    if (!wait_not_busy(500)) {
        return false;
    }
    uint32_t addr = sd_addr(lba);

    /* Pre-erase the block range.  This is optional but reduces the card's
       per-block programming latency on multi-block writes. */
    (void)cmd_begin(CMD55, 0, 0, false);
    (void)cmd_begin(CMD23, count, 0, false);

    uint8_t r = cmd_begin(CMD25, addr, 0, true);
    if (r != 0) {
        cs_high();
        return false;
    }

    for (uint32_t i = 0; i < count; i++) {
        if (i > 0) {
            /* Give the card time to assert its busy state after the previous
               data response before polling MISO. */
            sleep_us(100);
            if (!wait_not_busy(500)) {
                sd_abort_multi_write();
                return false;
            }
        }
        /* Each block of a multiple-block write is preceded by the 0xFC
           start-multiple-block token. */
        uint8_t start = DATA_TOKEN_START_MULTI;
        spi_write_blocking(CDRW_SD_SPI, &start, 1);
        const uint8_t *block = in + i * SD_BLOCK_SIZE;
        spi_write_dma(block, SD_BLOCK_SIZE);
        uint16_t crc = crc16_ccitt(block, SD_BLOCK_SIZE);
        uint8_t crc_bytes[2] = {(uint8_t)(crc >> 8), (uint8_t)crc};
        spi_write_blocking(CDRW_SD_SPI, crc_bytes, 2);

        uint8_t resp = 0xFF;
        for (int t = 0; t < 32; t++) {
            resp = xfer(0xFF);
            if (resp != 0xFF && (resp & 0x01)) {
                break;
            }
        }
        if ((resp & 0x1F) != DATA_RESP_ACCEPTED) {
            LOGF("SD: CMD25 data rejected lba=%lu block=%lu/%lu resp=%02X crc=%04X",
                 (unsigned long)lba, (unsigned long)i, (unsigned long)count,
                 resp, crc);
            sd_abort_multi_write();
            return false;
        }
    }

    sleep_us(200);
    (void)wait_not_busy(500);
    uint8_t stop = DATA_TOKEN_STOP;
    spi_write_blocking(CDRW_SD_SPI, &stop, 1);
    (void)xfer(0xFF);
    bool ok = wait_not_busy(1000);
    cs_high();
    return ok;
}

bool sd_write_blocks(uint32_t lba, const void *src, uint32_t count)
{
    if (!s_ready || !src || !count || !sd_card_present()) {
        return false;
    }
    if (s_blocks && (lba + count > s_blocks)) {
        return false;
    }
    const uint8_t *in = (const uint8_t *)src;
    spi_set_baudrate(CDRW_SD_SPI, s_write_hz);
    if (!s_write_hz_logged) {
        s_write_hz_logged = true;
        LOGF("SD: write clock=%lu Hz", (unsigned long)spi_get_baudrate(CDRW_SD_SPI));
    }

    if (count > 1 && s_cmd25_state != -1) {
        if (sd_write_multi(lba, in, count)) {
            s_cmd25_state = 1;
            return true;
        }
        /* A failed CMD25 leaves the card in multi-block write mode.  Reinit
           before falling back so the following CMD24 can be accepted. */
        s_cmd25_state = -1;
        if (s_write_hz > CDRW_SD_SPI_HZ) {
            s_write_hz = CDRW_SD_SPI_HZ;
            LOGF("SD: write clock fallback to 12 MHz");
        }
        LOGF("SD: CMD25 failed, reinit card and fallback CMD24");
        if (!sd_init()) {
            return false;
        }
        s_cmd25_state = -1;
        spi_set_baudrate(CDRW_SD_SPI, s_write_hz);
    }

    /* Single-block CMD24 fallback / normal path. */
    for (uint32_t i = 0; i < count; i++) {
        uint32_t addr = sd_addr(lba + i);
        if (!wait_not_busy(500)) {
            LOGF("SD: write pre-busy timeout lba=%lu block=%lu/%lu",
                 (unsigned long)lba, (unsigned long)i, (unsigned long)count);
            return false;
        }
        uint8_t r = cmd_begin(CMD24, addr, 0, true);
        if (r != 0) {
            LOGF("SD: write CMD24 failed lba=%lu block=%lu/%lu R1=%02X",
                 (unsigned long)lba, (unsigned long)i, (unsigned long)count, r);
            cs_high();
            return false;
        }
        (void)xfer(0xFF);
        uint8_t token = DATA_TOKEN_START;
        spi_write_blocking(CDRW_SD_SPI, &token, 1);
        const uint8_t *block = in + i * SD_BLOCK_SIZE;
        spi_write_dma(block, SD_BLOCK_SIZE);
        uint16_t crc = crc16_ccitt(block, SD_BLOCK_SIZE);
        uint8_t crc_bytes[2] = {(uint8_t)(crc >> 8), (uint8_t)crc};
        spi_write_blocking(CDRW_SD_SPI, crc_bytes, 2);

        uint8_t resp = 0xFF;
        for (int t = 0; t < 32; t++) {
            resp = xfer(0xFF);
            if (resp != 0xFF && (resp & 0x01)) {
                break;
            }
        }
        if ((resp & 0x1F) != DATA_RESP_ACCEPTED) {
            LOGF("SD: write data rejected lba=%lu block=%lu/%lu resp=%02X crc=%04X",
                 (unsigned long)lba, (unsigned long)i, (unsigned long)count,
                 resp, crc);
            cs_high();
            return false;
        }
        if (!wait_not_busy(1000)) {
            LOGF("SD: write busy timeout lba=%lu block=%lu/%lu",
                 (unsigned long)lba, (unsigned long)i, (unsigned long)count);
            cs_high();
            return false;
        }
        cs_high();
    }
    return true;
}

bool sd_sync(void)
{
    /* FatFs flushes its own cache via f_sync; every write is synchronous at
       the card level.  Return true so the caller can treat this as a no-op. */
    return s_ready;
}
