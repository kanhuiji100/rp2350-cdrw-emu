/*
 * Host-side BOT state-machine test for usb_cdrom.c.
 *
 * TinyUSB's endpoint calls are replaced with a tiny synchronous shim.  This
 * exercises CBW parsing, data phases, CSW generation and the SCSI glue, but
 * not the RP2350 USB controller itself.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tusb.h"
#include "class/msc/msc.h"
#include "device/usbd_pvt.h"
#include "media.h"
#include "scsi.h"
#include "usb_cdrom.h"
#include "util.h"

/* ---- media mock ------------------------------------------------------ */
static media_status_t g_media;
static uint8_t g_disc[100 * 2048];
static uint32_t g_last_write_lba;
static uint32_t g_last_write_blocks;

const media_status_t *media_get(void) { return &g_media; }
bool media_is_present(void) { return g_media.present; }
bool media_init(void) { return true; }
void media_shutdown(void) {}
bool media_load(const char *p) { (void)p; return true; }
bool media_reload(void) { return true; }
bool media_eject(void) { return true; }
bool media_create_blank(const char *p, uint32_t s) { (void)p; (void)s; return true; }
bool media_new_blank(char *p, size_t n) { (void)p; (void)n; return true; }
bool media_can_write(uint32_t l, uint32_t b) { (void)l; (void)b; return g_media.writable; }
bool media_read_blocks(uint32_t lba, void *dst, uint32_t blocks)
{
    memcpy(dst, g_disc + lba * 2048, blocks * 2048);
    return true;
}
bool media_write_blocks(uint32_t lba, const void *src, uint32_t blocks)
{
    memcpy(g_disc + lba * 2048, src, blocks * 2048);
    g_last_write_lba = lba;
    g_last_write_blocks = blocks;
    return true;
}
bool media_write_raw_blocks(uint32_t lba, const void *src, uint32_t blocks, uint32_t bs)
{
    (void)bs;
    return media_write_blocks(lba, src, blocks);
}
bool media_blank(void) { return true; }
bool media_format(void) { return true; }
bool media_format_capacity(uint32_t b) { (void)b; return true; }
bool media_is_formatted(void) { return false; }
bool media_is_formatting(void) { return false; }
bool media_is_erasing(void) { return false; }
bool media_close_session(bool f) { (void)f; return true; }
bool media_reserve_track(uint32_t l, uint32_t b) { (void)l; (void)b; return true; }
bool media_sync(void) { return true; }
bool media_save_meta(void) { return true; }
uint32_t media_default_capacity_lba(void) { return 100; }

/* ---- TinyUSB shims --------------------------------------------------- */
static const usbd_class_driver_t *g_drv;

static bool g_in_pending;
static uint8_t *g_in_buf;
static uint16_t g_in_len;
static uint8_t g_in_copy[65536];
static uint16_t g_in_copy_len;

static bool g_out_pending;
static uint8_t *g_out_buf;
static uint16_t g_out_len;

static bool g_stalled_in;
static bool g_stalled_out;

bool tud_connected(void) { return true; }
bool tud_mounted(void) { return true; }
bool tud_suspended(void) { return false; }

bool usbd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t *buffer, uint16_t total_bytes)
{
    (void)rhport;
    if (ep_addr & 0x80) {
        assert(!g_in_pending);
        g_in_pending = true;
        g_in_buf = buffer;
        g_in_len = total_bytes;
    } else {
        assert(!g_out_pending);
        g_out_pending = true;
        g_out_buf = buffer;
        g_out_len = total_bytes;
    }
    return true;
}

bool usbd_open_edpt_pair(uint8_t rhport, uint8_t const *p_desc, uint8_t ep_count,
                         uint8_t xfer_type, uint8_t *ep_out, uint8_t *ep_in)
{
    (void)rhport; (void)p_desc; (void)ep_count; (void)xfer_type;
    *ep_out = 0x01;
    *ep_in = 0x81;
    return true;
}

void usbd_edpt_stall(uint8_t rhport, uint8_t ep_addr)
{
    (void)rhport;
    if (ep_addr & 0x80) g_stalled_in = true;
    else g_stalled_out = true;
}

void usbd_edpt_clear_stall(uint8_t rhport, uint8_t ep_addr) { (void)rhport; (void)ep_addr; }
bool usbd_edpt_busy(uint8_t rhport, uint8_t ep_addr) { (void)rhport; (void)ep_addr; return false; }

bool tud_control_xfer(uint8_t rhport, const tusb_control_request_t *request, void *buffer, uint16_t len)
{
    (void)rhport; (void)request; (void)buffer; (void)len;
    return true;
}
bool tud_control_status(uint8_t rhport, const tusb_control_request_t *request)
{
    (void)rhport; (void)request;
    return true;
}

/* ---- BOT helpers ----------------------------------------------------- */
static uint32_t g_tag = 0x11223344;

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void send_cbw(const uint8_t *cdb, uint8_t cdb_len, uint32_t data_len, bool in)
{
    assert(g_out_pending && g_out_len == 31);
    uint8_t *cbw = g_out_buf;
    memset(cbw, 0, 31);
    put_le32(cbw, 0x43425355);
    put_le32(cbw + 4, ++g_tag);
    put_le32(cbw + 8, data_len);
    cbw[12] = in ? 0x80 : 0x00;
    cbw[13] = 0;
    cbw[14] = cdb_len;
    memcpy(cbw + 15, cdb, cdb_len);
    g_out_pending = false;
    g_drv->xfer_cb(0, 0x01, XFER_RESULT_SUCCESS, 31);
}

static bool complete_in(uint8_t *dst, uint16_t *len)
{
    if (!g_in_pending) return false;
    g_in_copy_len = g_in_len;
    memcpy(g_in_copy, g_in_buf, g_in_len);
    g_in_pending = false;
    if (dst && len) {
        memcpy(dst, g_in_copy, g_in_len);
        *len = g_in_len;
    }
    g_drv->xfer_cb(0, 0x81, XFER_RESULT_SUCCESS, g_in_len);
    return true;
}

static bool send_out(const void *data, uint16_t len)
{
    if (!g_out_pending || g_out_len < len) return false;
    memcpy(g_out_buf, data, len);
    g_out_pending = false;
    g_drv->xfer_cb(0, 0x01, XFER_RESULT_SUCCESS, len);
    return true;
}

static uint8_t csw_status(void)
{
    assert(g_in_pending && g_in_len == 13);
    uint8_t csw[13];
    uint16_t n = 0;
    assert(complete_in(csw, &n));
    assert(n == 13);
    assert(csw[0] == 0x55 && csw[1] == 0x53 && csw[2] == 0x42 && csw[3] == 0x53);
    return csw[12];
}

static void wait_cbw(void)
{
    assert(g_out_pending && g_out_len == 31);
}

static void setup_driver(void)
{
    uint8_t count = 0;
    g_drv = usbd_app_driver_get_cb(&count);
    assert(g_drv && count == 1);
    g_in_pending = false;
    g_out_pending = false;
    g_stalled_in = g_stalled_out = false;
    g_drv->init();
    static const uint8_t ifdesc[9] = {
        9, TUSB_DESC_INTERFACE, 0, 0, 2, TUSB_CLASS_MSC,
        MSC_SUBCLASS_SCSI, MSC_PROTOCOL_BOT, 0
    };
    /* The shim ignores the endpoint descriptors; 23 bytes is enough length. */
    uint16_t n = g_drv->open(0, (const tusb_desc_interface_t *)ifdesc, 23);
    assert(n == 23);
    wait_cbw();
}

static void reset_media(void)
{
    memset(&g_media, 0, sizeof(g_media));
    g_media.present = true;
    g_media.writable = true;
    g_media.kind = MEDIA_KIND_CDRW;
    g_media.capacity_lba = 100;
    g_media.blank = false;
    g_media.complete = true;
    g_media.data_lba = 10;
    g_media.nwa = 10;
    memset(g_disc, 0x5A, sizeof(g_disc));
    scsi_init();
    setup_driver();
}

static void test_inquiry_and_capacity(void)
{
    reset_media();
    uint8_t cdb[10] = {0x12, 0, 0, 0, 36, 0};
    send_cbw(cdb, 6, 36, true);
    uint8_t data[64];
    uint16_t n = 0;
    assert(complete_in(data, &n));
    assert(n == 36 && (data[0] & 0x1F) == 5);
    assert(csw_status() == 0);
    wait_cbw();

    uint8_t cap[10] = {0x25};
    send_cbw(cap, 10, 8, true);
    assert(complete_in(data, &n));
    assert(n == 8 && util_be32(data) == 99 && util_be32(data + 4) == 2048);
    assert(csw_status() == 0);
    wait_cbw();
}

static void test_read_write(void)
{
    reset_media();
    uint8_t read10[10] = {0x28, 0, 0, 0, 0, 0, 0, 0, 2, 0};
    send_cbw(read10, 10, 2 * 2048, true);
    uint8_t data[4096];
    uint16_t n = 0;
    assert(complete_in(data, &n));
    assert(n == 4096 && data[0] == 0x5A);
    assert(csw_status() == 0);
    wait_cbw();

    uint8_t block[2048];
    memset(block, 0xA7, sizeof(block));
    uint8_t write10[10] = {0x2A, 0, 0, 0, 0, 1, 0, 0, 1, 0};
    send_cbw(write10, 10, sizeof(block), false);
    assert(g_out_pending && g_out_len == 2048);
    assert(send_out(block, sizeof(block)));
    assert(csw_status() == 0);
    wait_cbw();
    assert(g_last_write_lba == 1 && g_last_write_blocks == 1);
    assert(memcmp(g_disc + 2048, block, sizeof(block)) == 0);
}

static void test_failed_write_and_unit_attention(void)
{
    reset_media();
    g_media.write_protected = true;
    uint8_t block[2048] = {0};
    uint8_t write10[10] = {0x2A, 0, 0, 0, 0, 0, 0, 0, 1, 0};
    send_cbw(write10, 10, sizeof(block), false);
    /* The failed command drains the OUT data before returning CSW. */
    assert(g_out_pending && g_out_len == sizeof(block));
    assert(send_out(block, sizeof(block)));
    assert(csw_status() == 1);
    wait_cbw();

    scsi_notify_media_change(true);
    uint8_t tur[6] = {0};
    send_cbw(tur, 6, 0, true);
    assert(csw_status() == 1);
    wait_cbw();

    uint8_t sense[6] = {0x03, 0, 0, 0, 18, 0};
    send_cbw(sense, 6, 18, true);
    uint8_t data[18];
    uint16_t n = 0;
    assert(complete_in(data, &n) && n == 18);
    assert(data[2] == 0x06 && data[12] == 0x28);
    assert(csw_status() == 0);
    wait_cbw();
}

int main(void)
{
    test_inquiry_and_capacity();
    test_read_write();
    test_failed_write_and_unit_attention();
    puts("USB BOT tests: all passed");
    return 0;
}
