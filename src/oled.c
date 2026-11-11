#include "oled.h"

#include <string.h>

#include "board.h"
#include "font5x7.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/time.h"

#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_PAGES (OLED_HEIGHT / 8)

static uint8_t s_fb[OLED_WIDTH * OLED_PAGES];
static bool s_present;

static bool i2c_write(uint8_t control, const uint8_t *data, size_t len)
{
    uint8_t chunk[33];
    while (len) {
        size_t n = len > 32 ? 32 : len;
        chunk[0] = control;
        memcpy(chunk + 1, data, n);
        if (i2c_write_blocking(CDRW_OLED_I2C, CDRW_OLED_ADDR, chunk,
                               (uint8_t)(n + 1), false) < 0) {
            return false;
        }
        data += n;
        len -= n;
    }
    return true;
}

static bool cmd(uint8_t c)
{
    return i2c_write(0x00, &c, 1);
}

bool oled_init(void)
{
    i2c_init(CDRW_OLED_I2C, CDRW_OLED_I2C_HZ);
    gpio_set_function(CDRW_OLED_SDA, GPIO_FUNC_I2C);
    gpio_set_function(CDRW_OLED_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(CDRW_OLED_SDA);
    gpio_pull_up(CDRW_OLED_SCL);

    static const uint8_t seq[] = {
        0xAE,       /* display off */
        0xD5, 0x80, /* clock divide */
        0xA8, 0x3F, /* multiplex 64 */
        0xD3, 0x00, /* display offset */
        0x40,       /* start line */
        0x8D, 0x14, /* charge pump on */
        0x20, 0x00, /* horizontal addressing */
        0xA1,       /* segment remap */
        0xC8,       /* COM scan remap */
        0xDA, 0x12, /* COM pins */
        0x81, 0xCF, /* contrast */
        0xD9, 0xF1, /* precharge */
        0xDB, 0x40, /* VCOM detect */
        0xA4,       /* resume RAM content */
        0xA6,       /* normal (not inverted) */
        0x2E,       /* no scroll */
        0xAF,       /* display on */
    };
    /* A missing OLED must not stop the USB drive, so probe it first by
       sending a harmless display-off command. */
    if (!cmd(0xAE)) {
        s_present = false;
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < sizeof(seq); i++) {
        ok = cmd(seq[i]) && ok;
    }
    s_present = ok;
    oled_clear();
    oled_flush();
    return ok;
}

bool oled_present(void)
{
    return s_present;
}

void oled_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

void oled_pixel(int x, int y, bool on)
{
    if ((unsigned)x >= OLED_WIDTH || (unsigned)y >= OLED_HEIGHT) {
        return;
    }
    uint32_t idx = (uint32_t)(y / 8) * OLED_WIDTH + (uint32_t)x;
    uint8_t mask = (uint8_t)(1u << (y & 7));
    if (on) s_fb[idx] |= mask;
    else s_fb[idx] &= (uint8_t)~mask;
}

void oled_hline(int x, int y, int w, bool on)
{
    for (int i = 0; i < w; i++) oled_pixel(x + i, y, on);
}

void oled_vline(int x, int y, int h, bool on)
{
    for (int i = 0; i < h; i++) oled_pixel(x, y + i, on);
}

void oled_rect(int x, int y, int w, int h, bool on)
{
    if (w <= 0 || h <= 0) return;
    oled_hline(x, y, w, on);
    oled_hline(x, y + h - 1, w, on);
    oled_vline(x, y, h, on);
    oled_vline(x + w - 1, y, h, on);
}

void oled_fill_rect(int x, int y, int w, int h, bool on)
{
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++) {
        oled_hline(x, yy, w, on);
    }
}

int oled_text_width(const char *s, int scale)
{
    if (!s || !*s) return 0;
    int n = (int)strlen(s);
    return n * 6 * scale - scale;
}

void oled_text(int x, int y, const char *s, int scale, bool color)
{
    if (!s) return;
    if (scale < 1) scale = 1;
    int cx = x;
    for (const char *p = s; *p; p++) {
        unsigned c = (unsigned char)*p;
        if (c < FONT5X7_FIRST || c > FONT5X7_LAST) c = '?';
        const uint8_t *glyph = font5x7[c - FONT5X7_FIRST];
        for (int col = 0; col < 5; col++) {
            uint8_t bits = glyph[col];
            for (int row = 0; row < 7; row++) {
                bool on = (bits >> row) & 1;
                if (scale == 1) {
                    oled_pixel(cx + col, y + row, on ? color : !color);
                } else {
                    oled_fill_rect(cx + col * scale, y + row * scale, scale,
                                   scale, on ? color : !color);
                }
            }
        }
        /* One-pixel inter-character gap. */
        for (int row = 0; row < 7 * scale; row++) {
            if (scale == 1) {
                oled_pixel(cx + 5, y + row, !color);
            } else {
                oled_fill_rect(cx + 5 * scale, y + row * scale, scale, scale, !color);
            }
        }
        cx += 6 * scale;
    }
}

void oled_text_centered(int y, const char *s, int scale, bool color)
{
    int w = oled_text_width(s, scale);
    oled_text((OLED_WIDTH - w) / 2, y, s, scale, color);
}

void oled_flush(void)
{
    if (!s_present) return;
    for (int page = 0; page < OLED_PAGES; page++) {
        (void)cmd((uint8_t)(0xB0 | page));
        (void)cmd(0x00);
        (void)cmd(0x10);
        (void)i2c_write(0x40, s_fb + page * OLED_WIDTH, OLED_WIDTH);
    }
}
