#include "encoder.h"

#include "board.h"
#include "hardware/gpio.h"
#include "pico/time.h"

#ifndef CDRW_ENCODER_TRANSITIONS_PER_DETENT
#define CDRW_ENCODER_TRANSITIONS_PER_DETENT 4
#endif

#define BTN_DEBOUNCE_MS 20u
#define BTN_LONG_MS 800u

static const int8_t s_quad_table[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0,
};

static uint8_t s_prev_state;
static int8_t s_accum;
static bool s_btn_raw;
static bool s_btn_stable;
static uint32_t s_btn_change_ms;
static bool s_btn_long_sent;
static uint32_t s_btn_down_ms;

void encoder_init(void)
{
    gpio_init(CDRW_ENC_A);
    gpio_set_dir(CDRW_ENC_A, GPIO_IN);
    gpio_pull_up(CDRW_ENC_A);
    gpio_init(CDRW_ENC_B);
    gpio_set_dir(CDRW_ENC_B, GPIO_IN);
    gpio_pull_up(CDRW_ENC_B);
    gpio_init(CDRW_ENC_BTN);
    gpio_set_dir(CDRW_ENC_BTN, GPIO_IN);
    gpio_pull_up(CDRW_ENC_BTN);

    s_prev_state = (uint8_t)((gpio_get(CDRW_ENC_A) ? 2 : 0) |
                             (gpio_get(CDRW_ENC_B) ? 1 : 0));
    s_btn_raw = gpio_get(CDRW_ENC_BTN);
    s_btn_stable = s_btn_raw;
    s_btn_change_ms = time_us_32() / 1000u;
}

void encoder_poll(encoder_event_t *event)
{
    event->delta = 0;
    event->pressed = false;
    event->long_press = false;

    uint8_t state = (uint8_t)((gpio_get(CDRW_ENC_A) ? 2 : 0) |
                              (gpio_get(CDRW_ENC_B) ? 1 : 0));
    if (state != s_prev_state) {
        s_accum = (int8_t)(s_accum + s_quad_table[(s_prev_state << 2) | state]);
        s_prev_state = state;
        if (s_accum >= CDRW_ENCODER_TRANSITIONS_PER_DETENT) {
            s_accum = 0;
            event->delta = 1;
        } else if (s_accum <= -CDRW_ENCODER_TRANSITIONS_PER_DETENT) {
            s_accum = 0;
            event->delta = -1;
        }
    }

    uint32_t now = time_us_32() / 1000u;
    bool raw = gpio_get(CDRW_ENC_BTN);
    if (raw != s_btn_raw) {
        s_btn_raw = raw;
        s_btn_change_ms = now;
    }
    if ((now - s_btn_change_ms) >= BTN_DEBOUNCE_MS && raw != s_btn_stable) {
        s_btn_stable = raw;
        if (!s_btn_stable) {
            s_btn_down_ms = now;
            s_btn_long_sent = false;
            event->pressed = true;
        }
    }
    if (!s_btn_stable && !s_btn_long_sent && (now - s_btn_down_ms) >= BTN_LONG_MS) {
        s_btn_long_sent = true;
        event->long_press = true;
    }
}
