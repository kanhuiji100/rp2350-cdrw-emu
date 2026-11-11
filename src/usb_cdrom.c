/*
 * USB Mass Storage Bulk-Only Transport class driver for an optical drive.
 *
 * This is intentionally not TinyUSB's stock MSC class: that class hard-codes
 * SCSI Peripheral Device Type 0 (direct access), which makes Windows/Linux
 * load the disk/removable-drive driver.  A real CD-RW must answer INQUIRY
 * with PDT 5 and implement the MMC command set, so we provide a compact BOT
 * endpoint state machine here and route SCSI through scsi.c.
 */
#include "usb_cdrom.h"

#include <stddef.h>
#include <string.h>

#include "class/msc/msc.h"
#include "debug_log.h"
#include "device/usbd_pvt.h"
#include "media.h"
#include "scsi.h"
#include "tusb.h"
#include "util.h"

#define BOT_CBW_SIZE 31u
#define BOT_CSW_SIZE 13u
#define BOT_CBW_SIGNATURE 0x43425355u
#define BOT_CSW_SIGNATURE 0x53425355u

#define BOT_STATUS_PASSED 0u
#define BOT_STATUS_FAILED 1u
#define BOT_STATUS_PHASE_ERROR 2u

#define MSC_REQ_GET_MAX_LUN 0xFE
#define MSC_REQ_RESET 0xFF

#define IO_CHUNK 32768u
#define OUT_CMD_MAX 2048u

typedef enum {
    STAGE_IDLE = 0,
    STAGE_CBW,
    STAGE_DATA_IN,
    STAGE_DATA_OUT_CMD,
    STAGE_DATA_OUT_WRITE,
    STAGE_DISCARD,
    STAGE_STATUS,
    STAGE_ERROR,
} bot_stage_t;

typedef enum {
    IN_NONE = 0,
    IN_META,
    IN_READ,
} in_kind_t;

static struct {
    uint8_t stage;
    uint8_t itf_num;
    uint8_t ep_in;
    uint8_t ep_out;

    uint8_t cbw[BOT_CBW_SIZE] __attribute__((aligned(4)));
    uint8_t csw[BOT_CSW_SIZE] __attribute__((aligned(4)));
    uint8_t scsi_buf[4096] __attribute__((aligned(4)));
    uint8_t out_cmd[OUT_CMD_MAX] __attribute__((aligned(4)));
    uint8_t io[IO_CHUNK] __attribute__((aligned(4)));
    uint8_t io2[IO_CHUNK] __attribute__((aligned(4)));

    uint32_t tag;
    uint32_t total;
    uint32_t done;
    uint8_t dir_in;
    uint8_t cdb_len;
    uint8_t cdb[16];
    uint8_t csw_status;
    uint8_t in_kind;

    uint32_t rd_lba;
    uint32_t rd_blocks;
    uint8_t *rd_buf;
    uint32_t rd_len;
    uint32_t rd_prefetch_off;
    uint32_t rd_prefetch_len;
    bool rd_prefetch_ready;
    bool rd_prefetch_failed;

    uint32_t wr_lba;
    uint32_t wr_blocks;
    uint32_t wr_src_size;
    uint32_t wr_chunk_max;
    uint8_t *wr_buf;

    bool failure_pending;
} s_usb;

static uint32_t s_last_activity_ms;

static usb_cdrom_trace_t s_trace[USB_CDROM_TRACE_DEPTH];
static uint8_t s_trace_head;
static uint8_t s_trace_count;
static int8_t s_trace_active = -1;

static void arm_cbw(void);

bool usb_cdrom_connected(void)
{
    return tud_connected();
}

bool usb_cdrom_configured(void)
{
    return tud_ready();
}

uint32_t usb_cdrom_last_activity_ms(void)
{
    return s_last_activity_ms;
}

void usb_cdrom_kick(void)
{
    /* No deferred work at present; the USB task is serviced in the mainloop. */
}

static void trace_record(uint8_t opcode, uint8_t dir_in, uint8_t cdb_len,
                         uint32_t data_len)
{
    uint8_t slot = s_trace_head;
    s_trace[slot].opcode = opcode;
    s_trace[slot].dir_in = dir_in;
    s_trace[slot].status = 0xFF; /* pending */
    s_trace[slot].cdb_len = cdb_len;
    s_trace[slot].data_len = data_len;
    s_trace[slot].lba = 0;
    s_trace[slot].blocks = 0;
    s_trace[slot].source_block_size = 0;
    s_trace[slot].key = 0;
    s_trace[slot].asc = 0;
    s_trace[slot].ascq = 0;
    s_trace[slot].discard = 0;
    s_trace_active = (int8_t)slot;
    s_trace_head = (uint8_t)((s_trace_head + 1) % USB_CDROM_TRACE_DEPTH);
    if (s_trace_count < USB_CDROM_TRACE_DEPTH) {
        s_trace_count++;
    }
}

uint8_t usb_cdrom_trace_count(void)
{
    return s_trace_count;
}

bool usb_cdrom_trace_get(uint8_t index, usb_cdrom_trace_t *out)
{
    if (!out || index >= s_trace_count) {
        return false;
    }
    uint8_t oldest = (uint8_t)((s_trace_head + USB_CDROM_TRACE_DEPTH -
                                s_trace_count) % USB_CDROM_TRACE_DEPTH);
    *out = s_trace[(oldest + index) % USB_CDROM_TRACE_DEPTH];
    return true;
}

static void mark_activity(void)
{
    s_last_activity_ms = util_millis();
}

static void bot_error(void)
{
    LOGF("BOT: protocol error");
    s_usb.stage = STAGE_ERROR;
    usbd_edpt_stall(0, s_usb.ep_in);
    usbd_edpt_stall(0, s_usb.ep_out);
}

static void send_csw(uint8_t status)
{
    uint32_t residue = s_usb.total > s_usb.done ? s_usb.total - s_usb.done : 0;
    memset(s_usb.csw, 0, BOT_CSW_SIZE);
    s_usb.csw[0] = (uint8_t)(BOT_CSW_SIGNATURE);
    s_usb.csw[1] = (uint8_t)(BOT_CSW_SIGNATURE >> 8);
    s_usb.csw[2] = (uint8_t)(BOT_CSW_SIGNATURE >> 16);
    s_usb.csw[3] = (uint8_t)(BOT_CSW_SIGNATURE >> 24);
    s_usb.csw[4] = (uint8_t)s_usb.tag;
    s_usb.csw[5] = (uint8_t)(s_usb.tag >> 8);
    s_usb.csw[6] = (uint8_t)(s_usb.tag >> 16);
    s_usb.csw[7] = (uint8_t)(s_usb.tag >> 24);
    s_usb.csw[8] = (uint8_t)residue;
    s_usb.csw[9] = (uint8_t)(residue >> 8);
    s_usb.csw[10] = (uint8_t)(residue >> 16);
    s_usb.csw[11] = (uint8_t)(residue >> 24);
    s_usb.csw[12] = status;
    s_usb.csw_status = status;
    const scsi_sense_t *sn = scsi_get_sense();
    uint8_t op = s_usb.cdb[0];
    bool is_read = (op == 0x28 || op == 0xA8 || op == 0xBE);
    bool log_csw = (status != BOT_STATUS_PASSED) || !is_read || CDRW_READ_TRACE;
    if (log_csw) {
        LOGF("CSW op=%02X st=%u residue=%lu sense=%02X/%02X/%02X",
             op, status, (unsigned long)residue, sn->key, sn->asc, sn->ascq);
        if (status != BOT_STATUS_PASSED && (op == 0x28 || op == 0xA8 || op == 0xBE)) {
            uint32_t rlba = util_be32(s_usb.cdb + 2);
            uint32_t rblk = (op == 0x28) ? util_be16(s_usb.cdb + 7)
                                        : util_be32(s_usb.cdb + 6);
            LOGF("READ FAIL op=%02X lba=%lu blocks=%lu", op,
                 (unsigned long)rlba, (unsigned long)rblk);
        }
    }
    if (s_trace_active >= 0) {
        s_trace[s_trace_active].status = status;
        const scsi_sense_t *sn = scsi_get_sense();
        s_trace[s_trace_active].key = sn->key;
        s_trace[s_trace_active].asc = sn->asc;
        s_trace[s_trace_active].ascq = sn->ascq;
    }
    s_usb.stage = STAGE_STATUS;
    (void)usbd_edpt_xfer(0, s_usb.ep_in, s_usb.csw, BOT_CSW_SIZE);
}

static void finish_ok(void)
{
    send_csw(BOT_STATUS_PASSED);
}

static void finish_fail(void)
{
    send_csw(BOT_STATUS_FAILED);
}

static void start_discard(bool fail_after)
{
    s_usb.failure_pending = fail_after;
    s_usb.stage = STAGE_DISCARD;
    uint32_t remaining = s_usb.total > s_usb.done ? s_usb.total - s_usb.done : 0;
    uint32_t chunk = remaining > IO_CHUNK ? IO_CHUNK : remaining;
    if (chunk == 0) {
        if (fail_after) finish_fail();
        else finish_ok();
        return;
    }
    (void)usbd_edpt_xfer(0, s_usb.ep_out, s_usb.io, (uint16_t)chunk);
}

static void send_meta_in(uint32_t len)
{
    if (len == 0) {
        finish_ok();
        return;
    }
    s_usb.in_kind = IN_META;
    s_usb.stage = STAGE_DATA_IN;
    (void)usbd_edpt_xfer(0, s_usb.ep_in, s_usb.scsi_buf, (uint16_t)len);
}

static bool read_chunk_into(uint8_t *buf, uint32_t off, uint32_t len)
{
    if ((len % SCSI_SECTOR_SIZE) != 0 || len == 0) {
        return false;
    }
    uint32_t blocks = len / SCSI_SECTOR_SIZE;
    uint32_t lba = s_usb.rd_lba + off / SCSI_SECTOR_SIZE;
    return media_read_blocks(lba, buf, blocks);
}

static void start_read_xfer(void)
{
    s_usb.in_kind = IN_READ;
    s_usb.stage = STAGE_DATA_IN;
    mark_activity();
    (void)usbd_edpt_xfer(0, s_usb.ep_in, s_usb.rd_buf, (uint16_t)s_usb.rd_len);
}

static void prefetch_read(void)
{
    if (s_usb.rd_prefetch_ready || s_usb.rd_prefetch_failed) {
        return;
    }
    if (s_usb.rd_prefetch_off >= s_usb.total) {
        return;
    }
    uint32_t len = s_usb.total - s_usb.rd_prefetch_off;
    if (len > IO_CHUNK) {
        len = IO_CHUNK;
    }
    uint8_t *buf = (s_usb.rd_buf == s_usb.io) ? s_usb.io2 : s_usb.io;
    if (!read_chunk_into(buf, s_usb.rd_prefetch_off, len)) {
        s_usb.rd_prefetch_failed = true;
        return;
    }
    s_usb.rd_prefetch_len = len;
    s_usb.rd_prefetch_off += len;
    s_usb.rd_prefetch_ready = true;
}

static void arm_write_chunk(void)
{
    uint32_t remaining = s_usb.total - s_usb.done;
    uint32_t chunk = remaining > s_usb.wr_chunk_max ? s_usb.wr_chunk_max : remaining;
    if (chunk == 0) {
        finish_ok();
        return;
    }
    s_usb.stage = STAGE_DATA_OUT_WRITE;
    (void)usbd_edpt_xfer(0, s_usb.ep_out, s_usb.wr_buf, (uint16_t)chunk);
}

static void handle_write_data(uint32_t xferred)
{
    uint32_t blocks = xferred / s_usb.wr_src_size;
    if (blocks == 0 || (xferred % s_usb.wr_src_size) != 0) {
        /* A short packet ended the transfer before a full source sector. */
        s_usb.done += xferred;
        finish_fail();
        return;
    }

    uint32_t lba = s_usb.wr_lba + (s_usb.done / s_usb.wr_src_size);
    uint8_t *buf = s_usb.wr_buf;
    s_usb.done += xferred;

    /* Start the next USB OUT before writing this chunk.  The host fills the
       alternate buffer while the SD card is programming the current one. */
    if (s_usb.done < s_usb.total) {
        s_usb.wr_buf = (buf == s_usb.io) ? s_usb.io2 : s_usb.io;
        arm_write_chunk();
    }

    if (!media_write_raw_blocks(lba, buf, blocks, s_usb.wr_src_size)) {
        LOGF("WRITE failed lba=%lu blocks=%lu", (unsigned long)lba,
             (unsigned long)blocks);
        scsi_set_sense(0x03, 0x0C, 0x00); /* MEDIUM ERROR / WRITE ERROR */
        finish_fail();
        return;
    }
    mark_activity();
    if (s_usb.done >= s_usb.total) {
        finish_ok();
    }
}

static void start_write(void)
{
    uint32_t source_block_size = 0;
    bool discard = false;
    bool ok = scsi_write_params(s_usb.cdb, s_usb.total, &s_usb.wr_lba,
                                &s_usb.wr_blocks, &source_block_size, &discard);
    const scsi_write_debug_t *wd = scsi_write_debug();
    if (s_trace_active >= 0) {
        s_trace[s_trace_active].lba = wd->lba;
        s_trace[s_trace_active].blocks = wd->blocks;
        s_trace[s_trace_active].source_block_size = wd->source_block_size;
        s_trace[s_trace_active].discard = wd->discard;
    }
    LOGF("WRITE lba=%lu blocks=%lu src=%lu discard=%d data=%lu",
         (unsigned long)wd->lba, (unsigned long)wd->blocks,
         (unsigned long)wd->source_block_size, wd->discard ? 1 : 0,
         (unsigned long)s_usb.total);
    if (!ok) {
        if (s_usb.total > 0) {
            start_discard(true);
        } else {
            finish_fail();
        }
        return;
    }
    if (s_usb.total == 0 || s_usb.wr_blocks == 0) {
        finish_ok();
        return;
    }
    if (s_usb.total != s_usb.wr_blocks * source_block_size) {
        finish_fail();
        return;
    }
    s_usb.wr_src_size = source_block_size;
    s_usb.wr_chunk_max = (IO_CHUNK / source_block_size) * source_block_size;
    if (s_usb.wr_chunk_max == 0) {
        finish_fail();
        return;
    }
    if (discard) {
        start_discard(false);
        return;
    }
    s_usb.done = 0;
    s_usb.wr_buf = s_usb.io;
    arm_write_chunk();
}

static void start_read(void)
{
    if (!scsi_read_params(s_usb.cdb, &s_usb.rd_lba, &s_usb.rd_blocks)) {
        finish_fail();
        return;
    }
    if (s_usb.total == 0 || s_usb.rd_blocks == 0) {
        finish_ok();
        return;
    }
    if (s_usb.total != s_usb.rd_blocks * SCSI_SECTOR_SIZE) {
        finish_fail();
        return;
    }
    s_usb.done = 0;
    s_usb.rd_buf = s_usb.io;
    s_usb.rd_len = s_usb.total > IO_CHUNK ? IO_CHUNK : s_usb.total;
    s_usb.rd_prefetch_off = s_usb.rd_len;
    s_usb.rd_prefetch_len = 0;
    s_usb.rd_prefetch_ready = false;
    s_usb.rd_prefetch_failed = false;
    if (!read_chunk_into(s_usb.rd_buf, 0, s_usb.rd_len)) {
        finish_fail();
        return;
    }
    start_read_xfer();
    /* Overlap the next SD read with the USB transfer just started. */
    prefetch_read();
}

static void start_cmd_out(void)
{
    if (s_usb.total == 0) {
        if (scsi_out_command(s_usb.cdb, NULL, 0) < 0) finish_fail();
        else finish_ok();
        return;
    }
    if (s_usb.total > sizeof(s_usb.out_cmd)) {
        start_discard(true);
        return;
    }
    s_usb.stage = STAGE_DATA_OUT_CMD;
    (void)usbd_edpt_xfer(0, s_usb.ep_out, s_usb.out_cmd, (uint16_t)s_usb.total);
}

static void handle_cbw(void)
{
    uint32_t signature = (uint32_t)s_usb.cbw[0] | ((uint32_t)s_usb.cbw[1] << 8) |
                         ((uint32_t)s_usb.cbw[2] << 16) | ((uint32_t)s_usb.cbw[3] << 24);
    uint32_t tag = (uint32_t)s_usb.cbw[4] | ((uint32_t)s_usb.cbw[5] << 8) |
                   ((uint32_t)s_usb.cbw[6] << 16) | ((uint32_t)s_usb.cbw[7] << 24);
    s_usb.tag = tag;
    s_usb.total = (uint32_t)s_usb.cbw[8] | ((uint32_t)s_usb.cbw[9] << 8) |
                  ((uint32_t)s_usb.cbw[10] << 16) | ((uint32_t)s_usb.cbw[11] << 24);
    s_usb.dir_in = (s_usb.cbw[12] & 0x80) ? 1 : 0;
    s_usb.cdb_len = s_usb.cbw[14] & 0x1F;
    s_usb.done = 0;
    s_usb.in_kind = IN_NONE;
    s_usb.failure_pending = false;
    mark_activity();

    if (signature != BOT_CBW_SIGNATURE || s_usb.cbw[13] != 0 ||
        s_usb.cdb_len < 1 || s_usb.cdb_len > 16) {
        bot_error();
        return;
    }
    memcpy(s_usb.cdb, s_usb.cbw + 15, 16);

    uint8_t opcode = s_usb.cdb[0];
    trace_record(opcode, s_usb.dir_in, s_usb.cdb_len, s_usb.total);
    bool is_read = (opcode == 0x28 || opcode == 0xA8 || opcode == 0xBE);
    bool log_cbw = !is_read;
    if (CDRW_READ_TRACE && is_read) {
        uint32_t nlba = util_be32(s_usb.cdb + 2);
        uint32_t nblk = (opcode == 0x28) ? util_be16(s_usb.cdb + 7)
                                        : util_be32(s_usb.cdb + 6);
        LOGF("READ op=%02X lba=%lu blocks=%lu len=%lu",
             opcode, (unsigned long)nlba, (unsigned long)nblk,
             (unsigned long)s_usb.total);
    } else if (log_cbw) {
        LOGF("CBW op=%02X dir=%s len=%lu cdb=%02X %02X %02X %02X %02X %02X",
             opcode, s_usb.dir_in ? "IN" : "OUT", (unsigned long)s_usb.total,
             s_usb.cdb[0], s_usb.cdb[1], s_usb.cdb[2], s_usb.cdb[3],
             s_usb.cdb[4], s_usb.cdb[5]);
    }
    if (scsi_is_write_command(opcode)) {
        if (s_usb.dir_in) {
            finish_fail();
        } else {
            start_write();
        }
        return;
    }
    if (scsi_is_read_command(opcode)) {
        if (!s_usb.dir_in) {
            finish_fail();
        } else {
            start_read();
        }
        return;
    }
    if (!s_usb.dir_in && s_usb.total > 0) {
        start_cmd_out();
        return;
    }

    int32_t rsp = scsi_in_command(s_usb.cdb, s_usb.scsi_buf, sizeof(s_usb.scsi_buf));
    if (rsp < 0) {
        finish_fail();
        return;
    }
    if (s_usb.total == 0 || rsp == 0) {
        if (s_usb.total > 0) {
            /* Host asked for IN data but the command has no response. */
            finish_fail();
        } else {
            finish_ok();
        }
        return;
    }
    uint32_t n = (uint32_t)rsp;
    if (n > s_usb.total) {
        n = s_usb.total;
    }
    s_usb.done = 0;
    send_meta_in(n);
}

static void arm_cbw(void)
{
    if (!s_usb.ep_out) {
        s_usb.stage = STAGE_IDLE;
        return;
    }
    s_usb.stage = STAGE_CBW;
    (void)usbd_edpt_xfer(0, s_usb.ep_out, s_usb.cbw, BOT_CBW_SIZE);
}

/* ---- TinyUSB class driver -------------------------------------------- */

static void cdrom_init(void)
{
    memset(&s_usb, 0, sizeof(s_usb));
    memset(s_trace, 0, sizeof(s_trace));
    s_trace_head = 0;
    s_trace_count = 0;
    s_trace_active = -1;
    s_usb.stage = STAGE_IDLE;
    s_last_activity_ms = util_millis();
}

static bool cdrom_deinit(void)
{
    return true;
}

static void cdrom_reset(uint8_t rhport)
{
    (void)rhport;
    s_usb.ep_in = 0;
    s_usb.ep_out = 0;
    s_usb.stage = STAGE_IDLE;
    s_usb.total = 0;
    s_usb.done = 0;
    scsi_bot_reset();
}

static uint16_t cdrom_open(uint8_t rhport, const tusb_desc_interface_t *itf_desc,
                           uint16_t max_len)
{
    if (itf_desc->bInterfaceClass != TUSB_CLASS_MSC ||
        itf_desc->bInterfaceSubClass != MSC_SUBCLASS_SCSI ||
        itf_desc->bInterfaceProtocol != MSC_PROTOCOL_BOT) {
        return 0;
    }
    uint16_t drv_len = (uint16_t)(sizeof(tusb_desc_interface_t) +
                                  2 * sizeof(tusb_desc_endpoint_t));
    if (max_len < drv_len) {
        return 0;
    }
    if (!usbd_open_edpt_pair(rhport, tu_desc_next(itf_desc), 2, TUSB_XFER_BULK,
                             &s_usb.ep_out, &s_usb.ep_in)) {
        return 0;
    }
    s_usb.itf_num = itf_desc->bInterfaceNumber;
    s_usb.stage = STAGE_IDLE;
    LOGF("USB: MSC CD-ROM interface opened (IN=%02X OUT=%02X)",
         s_usb.ep_in, s_usb.ep_out);
    arm_cbw();
    return drv_len;
}

static bool cdrom_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                  const tusb_control_request_t *request)
{
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }

    /* Let TinyUSB clear the halt first; we only need to restart the BOT. */
    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD &&
        request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_ENDPOINT &&
        request->bRequest == TUSB_REQ_CLEAR_FEATURE &&
        request->wValue == TUSB_REQ_FEATURE_EDPT_HALT) {
        uint8_t ep = tu_u16_low(request->wIndex);
        if (s_usb.stage == STAGE_ERROR) {
            s_usb.stage = STAGE_IDLE;
            s_usb.total = 0;
            s_usb.done = 0;
            scsi_bot_reset();
            arm_cbw();
        } else if (ep == s_usb.ep_out && s_usb.stage == STAGE_CBW &&
                   !usbd_edpt_busy(rhport, ep)) {
            arm_cbw();
        }
        return true;
    }

    if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_CLASS) {
        return false;
    }

    switch (request->bRequest) {
    case MSC_REQ_GET_MAX_LUN: {
        if (request->wValue != 0 || request->wLength != 1) {
            return false;
        }
        uint8_t max_lun = 0;
        return tud_control_xfer(rhport, request, &max_lun, 1);
    }
    case MSC_REQ_RESET: {
        if (request->wValue != 0 || request->wLength != 0) {
            return false;
        }
        s_usb.stage = STAGE_IDLE;
        s_usb.total = 0;
        s_usb.done = 0;
        scsi_bot_reset();
        usbd_edpt_clear_stall(rhport, s_usb.ep_in);
        usbd_edpt_clear_stall(rhport, s_usb.ep_out);
        arm_cbw();
        return tud_control_status(rhport, request);
    }
    default:
        return false;
    }
}

static bool cdrom_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result,
                          uint32_t xferred_bytes)
{
    (void)result;
    if (ep_addr == s_usb.ep_out && s_usb.stage == STAGE_CBW) {
        if (xferred_bytes != BOT_CBW_SIZE) {
            bot_error();
            return true;
        }
        handle_cbw();
        return true;
    }

    if (s_usb.stage == STAGE_DATA_IN && ep_addr == s_usb.ep_in) {
        if (s_usb.in_kind == IN_META) {
            s_usb.done = xferred_bytes;
            finish_ok();
        } else if (s_usb.in_kind == IN_READ) {
            if (xferred_bytes != s_usb.rd_len) {
                finish_fail();
                return true;
            }
            s_usb.done += xferred_bytes;
            if (s_usb.done >= s_usb.total) {
                finish_ok();
                return true;
            }
            if (s_usb.rd_prefetch_failed) {
                finish_fail();
                return true;
            }
            if (!s_usb.rd_prefetch_ready) {
                /* Fallback: read the next chunk synchronously. */
                uint32_t off = s_usb.done;
                uint32_t len = s_usb.total - off;
                if (len > IO_CHUNK) len = IO_CHUNK;
                uint8_t *buf = (s_usb.rd_buf == s_usb.io) ? s_usb.io2 : s_usb.io;
                if (!read_chunk_into(buf, off, len)) {
                    finish_fail();
                    return true;
                }
                s_usb.rd_buf = buf;
                s_usb.rd_len = len;
                s_usb.rd_prefetch_off = off + len;
                s_usb.rd_prefetch_ready = false;
                start_read_xfer();
                prefetch_read();
                return true;
            }
            /* Swap to the already-prefetched buffer, then refill the old one
               while the new USB transfer runs. */
            s_usb.rd_buf = (s_usb.rd_buf == s_usb.io) ? s_usb.io2 : s_usb.io;
            s_usb.rd_len = s_usb.rd_prefetch_len;
            s_usb.rd_prefetch_ready = false;
            start_read_xfer();
            prefetch_read();
        }
        return true;
    }

    if (s_usb.stage == STAGE_DATA_OUT_CMD && ep_addr == s_usb.ep_out) {
        s_usb.done += xferred_bytes;
        if (s_usb.done < s_usb.total) {
            /* out_cmd fits in one transfer by construction */
            finish_fail();
            return true;
        }
        if (scsi_out_command(s_usb.cdb, s_usb.out_cmd, s_usb.total) < 0) {
            finish_fail();
        } else {
            finish_ok();
        }
        return true;
    }

    if (s_usb.stage == STAGE_DATA_OUT_WRITE && ep_addr == s_usb.ep_out) {
        handle_write_data(xferred_bytes);
        return true;
    }

    if (s_usb.stage == STAGE_DISCARD && ep_addr == s_usb.ep_out) {
        s_usb.done += xferred_bytes;
        if (s_usb.done < s_usb.total) {
            start_discard(s_usb.failure_pending);
        } else if (s_usb.failure_pending) {
            finish_fail();
        } else {
            finish_ok();
        }
        return true;
    }

    if (s_usb.stage == STAGE_STATUS && ep_addr == s_usb.ep_in) {
        if (xferred_bytes == BOT_CSW_SIZE) {
            arm_cbw();
        } else {
            bot_error();
        }
        return true;
    }

    /* Ignore stale completion events (for example after a bus reset). */
    return true;
}

static const usbd_class_driver_t s_cdrom_driver = {
    .name = "MSC_CDROM",
    .init = cdrom_init,
    .deinit = cdrom_deinit,
    .reset = cdrom_reset,
    .open = cdrom_open,
    .control_xfer_cb = cdrom_control_xfer_cb,
    .xfer_cb = cdrom_xfer_cb,
    .sof = NULL,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = 1;
    return &s_cdrom_driver;
}
