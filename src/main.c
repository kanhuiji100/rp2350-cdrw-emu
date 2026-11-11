#include <stdio.h>
#include <string.h>

#include "board.h"
#include "debug_log.h"
#include "encoder.h"
#include "hardware/gpio.h"
#include "media.h"
#include "oled.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "scsi.h"
#include "storage.h"
#include "tusb.h"
#include "ui.h"
#include "usb_cdrom.h"
#include "util.h"
#include "version.h"

static bool s_led_on;
static uint32_t s_led_override_until;
static uint32_t s_last_activity_count;
static uint32_t s_last_sync_ms;
static uint32_t s_last_sd_retry_ms;
static bool s_usb_was_mounted;

static void led_init(void)
{
    gpio_init(CDRW_LED_PIN);
    gpio_set_dir(CDRW_LED_PIN, GPIO_OUT);
    gpio_put(CDRW_LED_PIN, CDRW_LED_ACTIVE_HIGH ? 0 : 1);
}

static void led_set(bool on)
{
    if (s_led_on == on) return;
    s_led_on = on;
    gpio_put(CDRW_LED_PIN, CDRW_LED_ACTIVE_HIGH ? on : !on);
}

static void boot_screen(const char *line2)
{
    oled_clear();
    oled_text_centered(14, "RP2350", 2, true);
    oled_text_centered(34, "CD-RW DRIVE", 1, true);
    oled_text_centered(50, line2, 1, true);
    oled_flush();
}

int main(void)
{
    stdio_init_all();
    sleep_ms(20);
    printf("\r\n\r\nRP2350 CD-RW emulator v%s\r\n", CDRW_VERSION);
    printf("debug UART: GP0 TX / GP1 RX, 115200 8N1\r\n");
    LOGF("boot: initializing hardware");
    led_init();

    bool oled_ok = oled_init();
    if (oled_ok) {
        boot_screen("booting...");
    }
    encoder_init();
    ui_init();

    media_init();
    scsi_init();

    char id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];
    pico_get_unique_board_id_string(id, sizeof(id));
    scsi_set_serial(id);

    bool sd_ok = storage_init();
    s_last_sd_retry_ms = util_millis();
    LOGF("storage: %s (error=%d)", sd_ok ? "ready" : "failed",
         (int)storage_last_error());
    if (oled_ok) {
        const char *msg = sd_ok ? "microSD ready" :
            (storage_last_error() == STORAGE_ERR_FAT_MOUNT ?
             "FAT32 mount failed" : "no microSD");
        boot_screen(msg);
    }

    if (sd_ok) {
        char path[STORAGE_PATH_MAX];
        if (storage_load_last_disc(path, sizeof(path))) {
            LOGF("boot: loading last disc %s", path);
            (void)media_load(path);
        }
    }

    tusb_init();
    LOGF("USB: Tusb initialized (full-speed device)");
    if (oled_ok) {
        boot_screen("USB 1.1 optical");
        sleep_ms(250);
    }

    s_last_sync_ms = util_millis();
    s_last_activity_count = 0;

    for (;;) {
        tud_task();
        ui_task();

        bool usb_mounted = tud_mounted();
        if (usb_mounted != s_usb_was_mounted) {
            s_usb_was_mounted = usb_mounted;
            LOGF("USB: %s", usb_mounted ? "host mounted" : "host disconnected");
        }

        uint32_t now = util_millis();

        /* The card may be inserted after power-up.  Retry every few seconds
           instead of requiring a reboot. */
        if (!storage_ready() && (now - s_last_sd_retry_ms) >= 5000u) {
            s_last_sd_retry_ms = now;
            LOGF("storage: retrying microSD");
            if (storage_init()) {
                LOGF("storage: ready after retry");
                char path[STORAGE_PATH_MAX];
                bool loaded = false;
                if (storage_load_last_disc(path, sizeof(path))) {
                    loaded = media_load(path);
                    LOGF("hotplug: last disc %s -> %s", path, loaded ? "loaded" : "failed");
                    if (loaded) {
                        scsi_notify_media_change(true);
                    }
                }
                ui_show_message(loaded ? "MICROSD READY" : "MICROSD READY");
            }
        }

        const media_status_t *m = media_get();
        uint32_t activity = m->read_count + m->write_count;
        if (activity != s_last_activity_count) {
            s_last_activity_count = activity;
            s_led_override_until = util_millis() + 80;
        }

        if (tud_mounted()) {
            if ((int32_t)(util_millis() - s_led_override_until) < 0) {
                led_set(true);
            } else {
                led_set(true);
            }
        } else {
            led_set((util_millis() / 500) & 1);
        }

        /* Persist CD-RW metadata and FatFs state periodically while the host
           is writing.  This is intentionally infrequent to avoid wearing the
           SD card and stealing USB bandwidth. */
        if ((now - s_last_sync_ms) >= 10000u) {
            s_last_sync_ms = now;
            if (media_is_present()) {
                (void)media_sync();
            }
        }

        tight_loop_contents();
    }
}
