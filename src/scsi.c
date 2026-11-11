/*
 * Portable SCSI MMC command subset for a single-session CD-RW/CD-ROM.
 * The command parser has no USB or Pico dependencies so it can also be built
 * and exercised by the host-side unit tests.
 */
#include "scsi.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "debug_log.h"
#include "media.h"
#include "util.h"

#define OP_TEST_UNIT_READY 0x00
#define OP_REQUEST_SENSE 0x03
#define OP_INQUIRY 0x12
#define OP_MODE_SELECT_6 0x15
#define OP_MODE_SENSE_6 0x1A
#define OP_START_STOP_UNIT 0x1B
#define OP_PREVENT_ALLOW 0x1E
#define OP_READ_FORMAT_CAPACITIES 0x23
#define OP_READ_CAPACITY_10 0x25
#define OP_READ_10 0x28
#define OP_WRITE_10 0x2A
#define OP_VERIFY_10 0x2F
#define OP_SYNCHRONIZE_CACHE_10 0x35
#define OP_READ_TOC 0x43
#define OP_READ_HEADER 0x44
#define OP_GET_CONFIGURATION 0x46
#define OP_GET_EVENT_STATUS 0x4A
#define OP_READ_DISC_INFORMATION 0x51
#define OP_READ_TRACK_INFORMATION 0x52
#define OP_RESERVE_TRACK 0x53
#define OP_SEND_OPC 0x54
#define OP_MODE_SELECT_10 0x55
#define OP_MODE_SENSE_10 0x5A
#define OP_CLOSE_TRACK_SESSION 0x5B
#define OP_READ_BUFFER 0x3C
#define OP_READ_BUFFER_CAPACITY 0x5C
#define OP_BLANK 0xA1
#define OP_READ_12 0xA8
#define OP_WRITE_12 0xAA
#define OP_GET_PERFORMANCE 0xAC
#define OP_READ_DISC_STRUCTURE 0xAD
#define OP_VERIFY_12 0xAF
#define OP_SET_CD_SPEED 0xBB
#define OP_MECHANISM_STATUS 0xBD
#define OP_READ_CD 0xBE

#define SENSE_NOT_READY 0x02
#define SENSE_MEDIUM_ERROR 0x03
#define SENSE_ILLEGAL_REQUEST 0x05
#define SENSE_UNIT_ATTENTION 0x06
#define SENSE_DATA_PROTECT 0x07

#define ASC_INVALID_COMMAND 0x20
#define ASC_LBA_OUT_OF_RANGE 0x21
#define ASC_INVALID_FIELD 0x24
#define ASC_WRITE_PROTECTED 0x27
#define ASC_MEDIUM_CHANGED 0x28
#define ASC_MEDIUM_NOT_PRESENT 0x3A
#define ASC_SAVING_PARAMS_NOT_SUPPORTED 0x39
#define ASC_MEDIUM_REMOVAL_PREVENTED 0x53

#define FEATURE_CORE 0x0001
#define FEATURE_MORPHING 0x0002
#define FEATURE_REMOVABLE 0x0003
#define FEATURE_WRITE_PROTECT 0x0004
#define FEATURE_RANDOM_READABLE 0x0010
#define FEATURE_MULTI_READ 0x001D
#define FEATURE_CD_READ 0x001E
#define FEATURE_INCREMENTAL_WRITE 0x0021
#define FEATURE_FORMATTABLE 0x0023
#define FEATURE_RESTRICTED_OVERWRITE 0x0026
#define FEATURE_CDRW_CAV 0x0027
#define FEATURE_CD_MASTERING 0x002E
#define FEATURE_TRACK_AT_ONCE 0x002D

#define PROFILE_CDROM 0x0008
#define PROFILE_CDRW 0x000A

typedef struct {
    scsi_sense_t sense;
    bool sense_valid;
    bool ua_pending;
    scsi_sense_t ua_sense;
    bool prevent;
    bool event_pending;
    uint8_t event_code;
    uint8_t write_type;
    uint8_t data_block_type;
    uint16_t read_speed_kbs;
    uint16_t write_speed_kbs;
    char serial[24];
} scsi_state_t;

static scsi_state_t s_scsi;
static scsi_write_debug_t s_write_debug;

static bool begin_command(uint8_t opcode)
{
    /* REQUEST SENSE must return the sense data left by the previous command,
       so it is the one command that must not clear it first. */
    if (opcode == OP_REQUEST_SENSE) {
        return true;
    }
    s_scsi.sense_valid = false;
    memset(&s_scsi.sense, 0, sizeof(s_scsi.sense));
    /* Capability/identity queries must remain usable across a media change;
       some burning applications issue GET CONFIGURATION before REQUEST SENSE
       and otherwise display "Current Profile: N/A" forever. */
    bool ua_exempt = opcode == OP_INQUIRY ||
                     opcode == OP_GET_EVENT_STATUS ||
                     opcode == OP_GET_CONFIGURATION ||
                     opcode == OP_MODE_SENSE_6 ||
                     opcode == OP_MODE_SENSE_10;
    if (s_scsi.ua_pending && !ua_exempt) {
        s_scsi.sense = s_scsi.ua_sense;
        s_scsi.sense_valid = true;
        /* Report the unit attention once.  A compliant host follows with
           REQUEST SENSE; a simpler burning app at least recovers on retry. */
        s_scsi.ua_pending = false;
        return false;
    }
    return true;
}

static int32_t fail(uint8_t key, uint8_t asc, uint8_t ascq)
{
    s_scsi.sense.key = key;
    s_scsi.sense.asc = asc;
    s_scsi.sense.ascq = ascq;
    s_scsi.sense_valid = true;
    return -1;
}

static int32_t require_media(void)
{
    const media_status_t *m = media_get();
    if (!m->present) {
        return fail(SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0);
    }
    return 0;
}

void scsi_init(void)
{
    memset(&s_scsi, 0, sizeof(s_scsi));
    s_scsi.read_speed_kbs = 4 * 176;
    s_scsi.write_speed_kbs = 4 * 176;
    s_scsi.write_type = 1;       /* Track-at-once */
    s_scsi.data_block_type = 8;  /* 2048-byte Mode 1 user data */
    util_safe_copy(s_scsi.serial, sizeof(s_scsi.serial), "RP2350CDRW0001");
}

void scsi_bot_reset(void)
{
    s_scsi.sense_valid = false;
    memset(&s_scsi.sense, 0, sizeof(s_scsi.sense));
}

void scsi_set_serial(const char *serial)
{
    if (serial) {
        util_safe_copy(s_scsi.serial, sizeof(s_scsi.serial), serial);
    }
}

const scsi_sense_t *scsi_get_sense(void)
{
    return &s_scsi.sense;
}

void scsi_set_sense(uint8_t key, uint8_t asc, uint8_t ascq)
{
    s_scsi.sense.key = key;
    s_scsi.sense.asc = asc;
    s_scsi.sense.ascq = ascq;
    s_scsi.sense_valid = true;
}

const scsi_write_debug_t *scsi_write_debug(void)
{
    return &s_write_debug;
}

bool scsi_ua_pending(void)
{
    return s_scsi.ua_pending;
}

bool scsi_media_locked(void)
{
    return s_scsi.prevent;
}

static void set_ua(uint8_t key, uint8_t asc, uint8_t ascq)
{
    s_scsi.ua_pending = true;
    s_scsi.ua_sense.key = key;
    s_scsi.ua_sense.asc = asc;
    s_scsi.ua_sense.ascq = ascq;
}

void scsi_notify_media_change(bool inserted)
{
    set_ua(SENSE_UNIT_ATTENTION, ASC_MEDIUM_CHANGED, 0);
    scsi_notify_media_event(inserted ? 3 : 4);
}

void scsi_notify_media_event(uint8_t event_code)
{
    s_scsi.event_code = event_code;
    s_scsi.event_pending = true;
}

bool scsi_is_read_command(uint8_t opcode)
{
    return opcode == OP_READ_10 || opcode == OP_READ_12 || opcode == OP_READ_CD;
}

bool scsi_is_write_command(uint8_t opcode)
{
    return opcode == OP_WRITE_10 || opcode == OP_WRITE_12 ||
           opcode == 0x2E || opcode == 0xAE; /* WRITE AND VERIFY */
}

static uint32_t rd10_lba(const uint8_t *cdb)
{
    return util_be32(cdb + 2);
}

static uint32_t rd10_blocks(const uint8_t *cdb)
{
    return util_be16(cdb + 7);
}

bool scsi_read_params(const uint8_t cdb[SCSI_CDB_MAX], uint32_t *lba, uint32_t *blocks)
{
    if (!begin_command(cdb[0])) {
        return false;
    }
    if (cdb[0] != OP_READ_10 && cdb[0] != OP_READ_12 &&
        cdb[0] != OP_READ_CD) {
        fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_COMMAND, 0);
        return false;
    }
    if (require_media() < 0) {
        return false;
    }
    const media_status_t *m = media_get();
    uint32_t l = rd10_lba(cdb);
    uint32_t n;
    if (cdb[0] == OP_READ_CD) {
        /* Support the common cooked-user-data form.  Main-channel selection
           bits for sync/header/ECC/subchannel are rejected rather than
           returning fabricated raw sectors. */
        uint8_t expected = (cdb[1] >> 2) & 0x07;
        uint8_t select = cdb[9];
        if ((expected != 0 && expected != 2 && expected != 4) ||
            (select & 0xF8) != 0x10 /* only User Data bit set */ ||
            (cdb[10] & 0x07) != 0) {
            fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
            return false;
        }
        n = ((uint32_t)cdb[6] << 16) | ((uint32_t)cdb[7] << 8) | cdb[8];
    } else {
        n = cdb[0] == OP_READ_10 ? rd10_blocks(cdb) : util_be32(cdb + 6);
    }
    if (n == 0) {
        *lba = l;
        *blocks = 0;
        return true;
    }
    /* CD lead-in/lead-out reads use virtual addresses outside the user
       area (negative LBA values such as -259, or high addresses such as
       0x37FFFEBE).  Real drives accept these probes.  Return a zero-filled
       cooked sector so IMAPI can finish its post-format verification. */
    if (l >= m->capacity_lba) {
        LOGF("READ lead-in/out lba=%lu blocks=%lu -> zero",
             (unsigned long)l, (unsigned long)n);
        *lba = 0;
        *blocks = n;
        return true;
    }
    if ((uint64_t)l + n > m->capacity_lba) {
        fail(SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0);
        return false;
    }
    *lba = l;
    *blocks = n;
    return true;
}

bool scsi_write_params(const uint8_t cdb[SCSI_CDB_MAX], uint32_t data_len,
                       uint32_t *lba, uint32_t *blocks, uint32_t *source_block_size,
                       bool *discard)
{
    memset(&s_write_debug, 0, sizeof(s_write_debug));
    if (discard) *discard = false;
    if (!begin_command(cdb[0])) {
        return false;
    }
    if (!scsi_is_write_command(cdb[0])) {
        fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_COMMAND, 0);
        return false;
    }
    const media_status_t *m = media_get();
    if (!m->present) {
        fail(SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0);
        return false;
    }
    if (!m->writable || m->write_protected) {
        fail(SENSE_DATA_PROTECT, ASC_WRITE_PROTECTED, 0);
        return false;
    }
    uint32_t l = rd10_lba(cdb);
    bool is12 = (cdb[0] == OP_WRITE_12 || cdb[0] == 0xAE);
    uint32_t n = is12 ? util_be32(cdb + 6) : rd10_blocks(cdb);
    s_write_debug.lba = l;
    s_write_debug.blocks = n;
    if (n == 0) {
        if (data_len != 0) {
            fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
            return false;
        }
        *lba = l;
        *blocks = 0;
        *source_block_size = SCSI_SECTOR_SIZE;
        return true;
    }
    if (data_len == 0 || data_len % n != 0) {
        fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
        return false;
    }
    uint32_t block_size = data_len / n;
    s_write_debug.source_block_size = block_size;
    switch (block_size) {
    case 2048:
    case 2056:
    case 2324:
    case 2332:
    case 2336:
    case 2352:
    case 2368:
    case 2448:
        break;
    default:
        fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
        return false;
    }
    uint64_t end = (uint64_t)l + n;
    if (end > m->capacity_lba) {
        /* Real CD-RW burners accept lead-in/lead-out writes that lie outside
           the user data area.  We cannot store them, but must consume the
           data and return GOOD so TAO/SAO sessions can proceed. */
        if (l >= m->capacity_lba) {
            s_write_debug.discard = 1;
            if (discard) *discard = true;
            *lba = l;
            *blocks = n;
            *source_block_size = block_size;
            return true;
        }
        fail(SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0);
        return false;
    }
    *lba = l;
    *blocks = n;
    *source_block_size = block_size;
    return true;
}

static void put_mode_page(uint8_t *buf, uint32_t *len, uint8_t page)
{
    const media_status_t *m = media_get();
    uint8_t *p = buf + *len;
    switch (page) {
    case 0x01: {
        p[0] = 0x01;
        p[1] = 0x0A;
        memset(p + 2, 0, 10);
        p[2] = 0x80; /* AWRE */
        p[3] = 0x20; /* read retry count */
        *len += 12;
        break;
    }
    case 0x05: {
        p[0] = 0x05;
        p[1] = 0x32;
        memset(p + 2, 0, 0x32);
        /* Captured CD-RW write parameters: packet write type 0, track
           mode 05h, link size 0, packet size 32. */
        p[2] = 0x40;
        p[3] = 0x05;
        p[4] = 8;
        p[5] = 0;
        p[12] = 0x20;
        p[14] = 150;
        *len += 2 + 0x32;
        break;
    }
    case 0x30: {
        p[0] = 0x30;
        p[1] = 0x0E;
        memset(p + 2, 0, 0x0E);
        p[2] = 0x03; /* BUEFS | TWBFS */
        p[3] = 0x01; /* BUEFE enabled */
        *len += 16;
        break;
    }
    case 0x08: {
        p[0] = 0x08;
        p[1] = 0x0A;
        memset(p + 2, 0, 10);
        p[2] = 0x04; /* WCE */
        *len += 12;
        break;
    }
    case 0x0D: {
        p[0] = 0x0D;
        p[1] = 0x06;
        memset(p + 2, 0, 6);
        p[4] = 0;
        p[5] = 60;
        p[6] = 0;
        p[7] = 75;
        *len += 8;
        break;
    }
    case 0x2A: {
        p[0] = 0x2A;
        p[1] = 0x1C; /* 28 bytes */
        memset(p + 2, 0, 28);
        p[2] = 0x07; /* CD-R read, CD-RW read, Method 2 */
        p[3] = (m->present && m->writable && !m->write_protected) ? 0x02 : 0x00;
        p[6] = 0x29; /* tray, eject, lock */
        p[8] = (uint8_t)(s_scsi.read_speed_kbs >> 8);
        p[9] = (uint8_t)s_scsi.read_speed_kbs;
        p[12] = 0;
        p[13] = 2; /* 2 KiB buffer */
        p[14] = (uint8_t)(s_scsi.read_speed_kbs >> 8);
        p[15] = (uint8_t)s_scsi.read_speed_kbs;
        p[16] = (uint8_t)(s_scsi.write_speed_kbs >> 8);
        p[17] = (uint8_t)s_scsi.write_speed_kbs;
        p[18] = (uint8_t)(s_scsi.write_speed_kbs >> 8);
        p[19] = (uint8_t)s_scsi.write_speed_kbs;
        p[28] = (uint8_t)(s_scsi.write_speed_kbs >> 8);
        p[29] = (uint8_t)s_scsi.write_speed_kbs;
        *len += 30;
        break;
    }
    default:
        break;
    }
}

static int32_t mode_sense(const uint8_t *cdb, uint8_t *buf, uint32_t cap, bool ten)
{
    const media_status_t *m = media_get();
    uint8_t page = cdb[2] & 0x3F;
    uint8_t pc = (cdb[2] >> 6) & 0x03;
    if (pc == 3) {
        return fail(SENSE_ILLEGAL_REQUEST, ASC_SAVING_PARAMS_NOT_SUPPORTED, 0);
    }
    memset(buf, 0, cap < 256 ? cap : 256);
    uint32_t header = ten ? 8 : 4;
    uint32_t len = header;
    bool wp = !m->present || !m->writable || m->write_protected;
    if (ten) {
        buf[3] = wp ? 0x80 : 0x00;
    } else {
        buf[2] = wp ? 0x80 : 0x00;
    }

    if (page == 0x3F) {
        static const uint8_t pages[] = {0x01, 0x05, 0x08, 0x0D, 0x2A};
        for (size_t i = 0; i < sizeof(pages); i++) {
            if (len + 64 >= cap) break;
            put_mode_page(buf, &len, pages[i]);
        }
    } else {
        if (page != 0x01 && page != 0x05 && page != 0x08 &&
            page != 0x0D && page != 0x2A && page != 0x30) {
            return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
        }
        if (len + 64 >= cap) {
            return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
        }
        put_mode_page(buf, &len, page);
    }

    if (ten) {
        buf[0] = (uint8_t)((len - 2) >> 8);
        buf[1] = (uint8_t)(len - 2);
    } else {
        if (len > 256) len = 256;
        buf[0] = (uint8_t)(len - 1);
    }
    return (int32_t)len;
}

static int32_t inquiry(const uint8_t *cdb, uint8_t *buf, uint32_t cap)
{
    bool evpd = (cdb[1] & 1) != 0;
    uint8_t page = cdb[2];
    uint32_t alloc = util_be16(cdb + 3);
    (void)cap;
    if (!evpd) {
        if (page != 0) {
            return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
        }
        memset(buf, 0, 36);
        buf[0] = 0x05; /* peripheral qualifier 0, device type CD-ROM */
        buf[1] = 0x80; /* removable */
        buf[2] = 0x05; /* SPC-3 */
        buf[3] = 0x02;
        buf[4] = 31;
        memcpy(buf + 8, "RP2350  ", 8);
        memcpy(buf + 16, "CD-RW USB 1.1   ", 16);
        memcpy(buf + 32, "1.00", 4);
        return alloc < 36 ? (int32_t)alloc : 36;
    }

    if (page == 0x00) {
        uint8_t pages[] = {0x00, 0x80, 0x83};
        buf[0] = 0x05;
        buf[1] = 0x00;
        buf[2] = 0x00;
        buf[3] = (uint8_t)(sizeof(pages) + 1);
        memcpy(buf + 4, pages, sizeof(pages));
        uint32_t n = 4 + sizeof(pages);
        return alloc < n ? (int32_t)alloc : (int32_t)n;
    }
    if (page == 0x80) {
        size_t slen = strlen(s_scsi.serial);
        if (slen > 32) slen = 32;
        buf[0] = 0x05;
        buf[1] = 0x80;
        buf[2] = 0x00;
        buf[3] = (uint8_t)slen;
        memset(buf + 4, 0, 32);
        memcpy(buf + 4, s_scsi.serial, slen);
        uint32_t n = 4 + (uint32_t)slen;
        return alloc < n ? (int32_t)alloc : (int32_t)n;
    }
    if (page == 0x83) {
        /* One T10 vendor identification designator. */
        const char *id = "RP2350_CDRW_0001";
        uint32_t idlen = (uint32_t)strlen(id);
        buf[0] = 0x05;
        buf[1] = 0x83;
        buf[2] = 0;
        buf[3] = (uint8_t)(4 + idlen);
        buf[4] = 0x02; /* ASCII */
        buf[5] = 0x01; /* T10 vendor ID */
        buf[6] = 0x00;
        buf[7] = (uint8_t)idlen;
        memcpy(buf + 8, id, idlen);
        uint32_t n = 8 + idlen;
        return alloc < n ? (int32_t)alloc : (int32_t)n;
    }
    return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
}

static uint32_t current_profile(void)
{
    const media_status_t *m = media_get();
    if (!m->present) return 0;
    return m->kind == MEDIA_KIND_ISO ? PROFILE_CDROM : PROFILE_CDRW;
}

static void feature_begin(uint8_t *buf, uint32_t *len, uint16_t code,
                          uint8_t version, bool persistent, bool current,
                          uint8_t add_len)
{
    uint8_t *p = buf + *len;
    p[0] = (uint8_t)(code >> 8);
    p[1] = (uint8_t)code;
    p[2] = (uint8_t)((version << 2) | (persistent ? 2 : 0) | (current ? 1 : 0));
    p[3] = add_len;
    *len += 4 + add_len;
}

static int32_t get_configuration(const uint8_t *cdb, uint8_t *buf, uint32_t cap)
{
    (void)cap;
    const media_status_t *m = media_get();
    bool has_media = m->present;
    bool cdrw = has_media && m->kind == MEDIA_KIND_CDRW;
    bool writable = cdrw && m->writable && !m->write_protected;
    uint16_t start_feature = util_be16(cdb + 2);
    uint8_t body[400];
    uint32_t len = 0;
    memset(body, 0, sizeof(body));

#define WANT(feature) ((start_feature) == 0 || (feature) >= (start_feature))
    if (WANT(0x0000)) {
        bool cur_cdrom = current_profile() == PROFILE_CDROM;
        bool cur_cdrw = current_profile() == PROFILE_CDRW;
        feature_begin(body, &len, 0x0000, 0, true, true, 8);
        uint8_t *p = body + len - 8;
        util_put_be16(p + 0, PROFILE_CDRW);
        p[2] = cur_cdrw ? 1 : 0;
        util_put_be16(p + 4, PROFILE_CDROM);
        p[6] = cur_cdrom ? 1 : 0;
    }
    if (WANT(FEATURE_CORE)) {
        feature_begin(body, &len, FEATURE_CORE, 0, true, true, 4);
        uint8_t *p = body + len - 4;
        util_put_be32(p, 1); /* SCSI family physical interface */
    }
    if (WANT(FEATURE_MORPHING)) {
        feature_begin(body, &len, FEATURE_MORPHING, 1, true, true, 4);
    }
    if (WANT(FEATURE_REMOVABLE)) {
        feature_begin(body, &len, FEATURE_REMOVABLE, 0, true, true, 4);
        body[len - 4] = 0x29; /* tray, eject, lock, prevent jumper present */
    }
    if (WANT(FEATURE_WRITE_PROTECT)) {
        feature_begin(body, &len, FEATURE_WRITE_PROTECT, 0, true,
                      has_media && (!m->writable || m->write_protected), 4);
    }
    if (WANT(FEATURE_RANDOM_READABLE) && has_media) {
        feature_begin(body, &len, FEATURE_RANDOM_READABLE, 0, true, true, 8);
        uint8_t *p = body + len - 8;
        util_put_be32(p, SCSI_SECTOR_SIZE);
        util_put_be16(p + 4, 1);
        p[6] = 0x01; /* PP: Read/Write Error Recovery page is present */
    }
    if (WANT(FEATURE_MULTI_READ) && has_media) {
        feature_begin(body, &len, FEATURE_MULTI_READ, 0, true, true, 0);
    }
    if (WANT(FEATURE_CD_READ) && has_media) {
        feature_begin(body, &len, FEATURE_CD_READ, 1, true, true, 4);
    }
    if (WANT(FEATURE_INCREMENTAL_WRITE) && writable) {
        feature_begin(body, &len, FEATURE_INCREMENTAL_WRITE, 1, false, true, 8);
        uint8_t *p = body + len - 8;
        util_put_be16(p, 0x0100); /* data block type 8 (2048-byte) */
        p[2] = 0x01;              /* BUF */
        p[3] = 1;                 /* one link size */
        p[4] = 7;
    }
    if (WANT(FEATURE_FORMATTABLE) && writable) {
        feature_begin(body, &len, FEATURE_FORMATTABLE, 0, false, true, 0);
    }
    if (WANT(FEATURE_RESTRICTED_OVERWRITE) && writable) {
        feature_begin(body, &len, FEATURE_RESTRICTED_OVERWRITE, 0, false, true, 0);
    }
    if (WANT(FEATURE_CDRW_CAV) && cdrw) {
        feature_begin(body, &len, FEATURE_CDRW_CAV, 0, false, writable, 4);
    }
    if (WANT(FEATURE_TRACK_AT_ONCE) && cdrw) {
        feature_begin(body, &len, FEATURE_TRACK_AT_ONCE, 2, false, writable, 4);
        uint8_t *p = body + len - 4;
        p[0] = 0x42; /* BUF + CD-RW */
        util_put_be16(p + 2, 0x0100);
    }
    if (WANT(FEATURE_CD_MASTERING) && writable) {
        feature_begin(body, &len, FEATURE_CD_MASTERING, 0, false, true, 4);
        uint8_t *p = body + len - 4;
        p[0] = 0x01; /* SAO supported */
        p[1] = 0x00;
        util_put_be16(p + 2, 0x0400); /* max cue sheet size */
    }
#undef WANT

    memset(buf, 0, 8);
    util_put_be32(buf, len + 4);
    util_put_be16(buf + 6, current_profile());
    if (len > sizeof(body)) len = sizeof(body);
    memcpy(buf + 8, body, len);
    return (int32_t)(8 + len);
}

static int32_t read_disc_information(uint8_t *buf)
{
    const media_status_t *m = media_get();
    if (require_media() < 0) return -1;
    memset(buf, 0, 36);
    util_put_be16(buf, 32);
    bool really_blank = m->blank && m->data_lba == 0;
    uint8_t erasable = (m->writable && !m->write_protected) ? 0x10 : 0;
    uint8_t state = really_blank ? 0 : (m->complete ? 3 : 1);
    uint8_t status = really_blank ? 0 : (m->complete ? 2 : 1);
    buf[2] = erasable | (uint8_t)(state << 2) | status;
    buf[3] = 1;
    buf[4] = 1;
    buf[5] = 1;
    buf[6] = 1;
    buf[7] = m->complete ? 0x80 : 0x00;
    buf[8] = m->complete ? 0x00 : 0xFF;
    if (m->complete) {
        util_put_be32(buf + 12, 0x00029900);
        util_put_be32(buf + 16, 0xFFFFFFFFu);
        util_put_be32(buf + 20, 0xFFFFFFFFu);
    } else if (really_blank) {
        util_put_be32(buf + 16, 0x00612217);
        buf[20] = 0;
        buf[21] = 0x4F;
        buf[22] = 0x3B;
        buf[23] = 0x4A;
    } else {
        util_put_be32(buf + 16, 0xFFFFFFFFu);
        util_put_be32(buf + 20, 0xFFFFFFFFu);
    }
    LOGF("DISCINFO blank=%d complete=%d data=%lu sessions=%u state=%u status=%u",
         m->blank ? 1 : 0, m->complete ? 1 : 0, (unsigned long)m->data_lba,
         buf[4], state, status);
    return m->complete ? 34 : 36;
}

static int32_t read_track_information(const uint8_t *cdb, uint8_t *buf)
{
    const media_status_t *m = media_get();
    if (require_media() < 0) return -1;
    uint8_t type = cdb[1] & 0x03;
    uint32_t number = util_be32(cdb + 2);
    if (type == 0) {
        if (number >= m->capacity_lba) {
            return fail(SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0);
        }
    } else if (type == 1) {
        if (!(number == 0 || number == 1 || number == 0xFF)) {
            return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
        }
    } else if (type == 2) {
        if (number != 1) {
            return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
        }
    } else {
        return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
    }

    if (m->kind == MEDIA_KIND_CDRW && m->data_lba == 0 &&
        (m->blank || m->formatted) && type == 1 && number == 1) {
        memset(buf, 0, 48);
        util_put_be16(buf, 46);
        buf[2] = 1;
        buf[3] = 1;
        buf[5] = 0x05;
        if (m->complete) {
            buf[6] = 0xB1;
            buf[7] = 0x02;
            util_put_be32(buf + 12, 0xFFFFFFFFu);
            util_put_be32(buf + 20, 0x20);
            util_put_be32(buf + 24, m->capacity_lba);
            util_put_be32(buf + 28, m->capacity_lba ? m->capacity_lba - 1 : 0);
            return 36;
        }
        buf[6] = 0x4F;
        buf[7] = 0x01;
        uint32_t nwa = m->formatted
                           ? 0
                           : (m->capacity_lba > 2 ? m->capacity_lba - 2 : 0);
        util_put_be32(buf + 20, nwa);
        util_put_be32(buf + 28, nwa);
        util_put_be32(buf + 32, m->formatted ? m->capacity_lba : 0xFFFFFFFFu);
        return 48;
    }

    memset(buf, 0, 36);
    util_put_be16(buf, 34);
    buf[2] = 1;
    buf[3] = 1;
    buf[5] = 0x04;
    bool writable = m->writable && !m->write_protected;
    bool really_blank = m->data_lba == 0;
    buf[6] = (really_blank ? 0x40 : 0x00) | 0x01;
    bool has_recorded = m->data_lba > 0;
    buf[7] = (has_recorded ? 0x02 : 0x00) |
             ((writable && (!m->complete || m->data_lba == 0)) ? 0x01 : 0x00);
    util_put_be32(buf + 8, 0);
    util_put_be32(buf + 12, m->data_lba);
    uint32_t free_blocks = 0;
    if (writable && (!m->complete || m->data_lba == 0)) {
        free_blocks = m->capacity_lba > m->data_lba ? m->capacity_lba - m->data_lba : 0;
    }
    util_put_be32(buf + 16, free_blocks);
    util_put_be32(buf + 20, 16);
    util_put_be32(buf + 24, m->data_lba ? m->data_lba : m->capacity_lba);
    util_put_be32(buf + 28, m->data_lba ? m->data_lba - 1 : 0);
    return 36;
}

static void toc_address(uint8_t *buf, bool msf, uint32_t lba)
{
    if (msf) {
        uint32_t x = lba + 150;
        buf[0] = 0;
        buf[1] = (uint8_t)(x / (75 * 60));
        buf[2] = (uint8_t)((x / 75) % 60);
        buf[3] = (uint8_t)(x % 75);
    } else {
        util_put_be32(buf, lba);
    }
}

static int32_t read_toc(const uint8_t *cdb, uint8_t *buf)
{
    const media_status_t *m = media_get();
    if (require_media() < 0) return -1;
    bool msf = (cdb[1] & 0x02) != 0;
    uint8_t format = cdb[2] & 0x0F;
    uint8_t start_track = cdb[6];
    if ((cdb[1] & ~0x02) != 0 || (start_track > 1 && format != 1)) {
        return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
    }
    bool formatted_empty = m->blank && m->formatted && m->data_lba == 0;
    uint32_t toc_capacity = m->nominal_lba ? m->nominal_lba : m->capacity_lba;
    toc_capacity = (format == 0) ? 359336u : 359848u;
    uint8_t toc_ctrl = 0x15; /* CD-RW: ADR=1, data, control 15h */
    uint32_t leadout = ((m->session_open || m->complete || formatted_empty) &&
                        m->data_lba == 0)
                           ? toc_capacity
                           : m->data_lba;
    LOGF("TOC fmt=%u leadout=%lu blank=%d complete=%d data=%lu",
         format, (unsigned long)leadout, m->blank ? 1 : 0,
         m->complete ? 1 : 0, (unsigned long)m->data_lba);
    if ((m->blank || (m->complete && m->data_lba == 0)) &&
        (format == 0 || format == 1 || format == 2)) {
        return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
    }
    if (format == 0) {
        uint32_t len = 20;
        memset(buf, 0, len);
        buf[1] = (uint8_t)(len - 2);
        buf[2] = 1;
        buf[3] = 1;
        buf[5] = toc_ctrl;
        buf[6] = 1;
        toc_address(buf + 8, msf, 0);
        buf[13] = toc_ctrl;
        buf[14] = 0xAA;
        toc_address(buf + 16, msf, leadout);
        return (int32_t)len;
    }
    if (format == 1) {
        uint32_t len = 12;
        memset(buf, 0, len);
        buf[1] = 10;
        buf[2] = 1;
        buf[3] = 1;
        buf[5] = toc_ctrl;
        buf[6] = 1;
        toc_address(buf + 8, msf, 0);
        return (int32_t)len;
    }
    if (format == 2) {
        uint32_t len = 4 + 4 * 11;
        memset(buf, 0, len);
        buf[1] = (uint8_t)(len - 2);
        buf[2] = 1;
        buf[3] = 1;
        uint8_t *p = buf + 4;
        for (int i = 0; i < 3; i++) {
            p[0] = 1;
            p[1] = toc_ctrl;
            p[2] = 0;
            p[3] = (uint8_t)(0xA0 + i);
            if (i == 0 || i == 1) {
                p[8] = 1;
            } else {
                p[8] = (uint8_t)((leadout + 150) / (75 * 60));
                p[9] = (uint8_t)(((leadout + 150) / 75) % 60);
                p[10] = (uint8_t)((leadout + 150) % 75);
            }
            p += 11;
        }
        p[0] = 1;
        p[1] = toc_ctrl;
        p[2] = 1;
        p[3] = 1;
        p[8] = 0;
        p[9] = 2;
        p[10] = 0;
        return (int32_t)len;
    }
    if (format == 3) {
        memset(buf, 0, 4);
        buf[1] = 2;
        return 4;
    }
    if (format == 4) {
        if (m->kind != MEDIA_KIND_CDRW) {
            return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
        }
        uint32_t len = 4 + 28;
        memset(buf, 0, len);
        buf[1] = 30;
        uint8_t *p = buf + 4;
        p[0] = 0xC2;
        p[1] = 0x40;
        p[2] = 0xC4;
        p[3] = 0;
        p[4] = 97;
        p[5] = 26;
        p[6] = 50;
        p[7] = 0;
        uint32_t lo = m->capacity_lba + 150u;
        p[8] = (uint8_t)(lo / (75u * 60u));
        p[9] = (uint8_t)((lo / 75u) % 60u);
        p[10] = (uint8_t)(lo % 75u);
        p[11] = 0;
        return (int32_t)len;
    }
    return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
}

static int32_t get_event_status(const uint8_t *cdb, uint8_t *buf)
{
    bool polled = (cdb[1] & 0x01) != 0;
    uint8_t classes = cdb[4];
    uint16_t alloc = util_be16(cdb + 7);
    if (!polled) {
        return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
    }
    memset(buf, 0, 8);
    if (classes == 0) {
        buf[0] = 0;
        buf[1] = 2;
        buf[2] = 0x80;
        buf[3] = 0x5E;
        uint32_t n = alloc < 4 ? alloc : 4;
        return (int32_t)n;
    }
    if (classes == 0x5A || classes == 0x10) {
        util_put_be16(buf, 6);
        buf[2] = 0x06;
        buf[3] = 0x5E;
        buf[4] = s_scsi.event_pending ? 0x02 : 0x00;
        buf[5] = 1;
        s_scsi.event_pending = false;
        s_scsi.event_code = 0;
        uint32_t n = alloc < 8 ? alloc : 8;
        return (int32_t)n;
    }
    buf[0] = 0;
    buf[1] = 2;
    buf[2] = 0x80;
    buf[3] = 0x5E;
    uint32_t n = alloc < 4 ? alloc : 4;
    return (int32_t)n;
}

static int32_t mechanism_status(uint8_t *buf)
{
    memset(buf, 0, 8);
    buf[5] = 0x01; /* CD-ROM mechanism */
    return 8;
}

static int32_t read_capacity(uint8_t *buf)
{
    const media_status_t *m = media_get();
    if (require_media() < 0) return -1;
    memset(buf, 0, 8);
    util_put_be32(buf, m->capacity_lba ? m->capacity_lba - 1 : 0);
    util_put_be32(buf + 4, SCSI_SECTOR_SIZE);
    return 8;
}

static int32_t read_format_capacities(uint8_t *buf)
{
    const media_status_t *m = media_get();
    if (require_media() < 0) return -1;
    uint32_t physical = m->nominal_lba ? m->nominal_lba : m->capacity_lba;
    uint32_t fmtcap = 295264;
    if (physical < fmtcap) fmtcap = physical;
    memset(buf, 0, 4 + 4 * 8);
    util_put_be32(buf, 32);
    uint8_t *d = buf + 4;
    util_put_be32(d, physical);
    d[4] = m->blank ? 0x01 : 0x02;
    d[6] = (uint8_t)(SCSI_SECTOR_SIZE >> 8);
    d[7] = (uint8_t)SCSI_SECTOR_SIZE;
    d += 8;
    util_put_be32(d, fmtcap);
    d[4] = 0x40; /* type 10h, formatted capacity */
    d[7] = 0x20;
    d += 8;
    d[4] = 0x44;
    d[7] = 0x20;
    d += 8;
    d[4] = 0x48;
    d[7] = 0x20;
    return 36;
}

static int32_t request_sense(uint8_t *buf)
{
    memset(buf, 0, 18);
    buf[0] = 0x70;
    buf[2] = s_scsi.sense_valid ? (s_scsi.sense.key & 0x0F) : 0;
    buf[7] = 10;
    buf[12] = s_scsi.sense_valid ? s_scsi.sense.asc : 0;
    buf[13] = s_scsi.sense_valid ? s_scsi.sense.ascq : 0;
    s_scsi.sense_valid = false;
    s_scsi.ua_pending = false;
    memset(&s_scsi.sense, 0, sizeof(s_scsi.sense));
    return 18;
}

static int32_t format_unit(const uint8_t *cdb, const uint8_t *data, uint32_t len)
{
    if (require_media() < 0) return -1;
    const media_status_t *m = media_get();
    if (!m->writable || m->write_protected) {
        return fail(SENSE_DATA_PROTECT, ASC_WRITE_PROTECTED, 0);
    }
    uint32_t blocks = 0;
    if (data && len >= 8) {
        uint32_t b = util_be32(data + 4);
        if (b >= 100 && b <= m->nominal_lba) blocks = b;
    }
    if (!media_format_capacity(blocks)) {
        return fail(SENSE_MEDIUM_ERROR, 0x0C, 0x00);
    }
    /* Format completion is reported as media event 07h.  Even for an
       immediate FORMAT UNIT, report the event so IMAPI can finish. */
    s_scsi.event_code = 0x07;
    s_scsi.event_pending = true;
    LOGF("SCSI: FORMAT UNIT len=%lu data=%02X %02X %02X %02X %02X %02X",
         (unsigned long)len, data ? data[0] : 0, data ? data[1] : 0,
         data ? data[2] : 0, data ? data[3] : 0, data ? data[4] : 0,
         data ? data[5] : 0);
    (void)cdb;
    return 0;
}

static int32_t start_stop(const uint8_t *cdb)
{
    uint8_t power = cdb[4] & 0xF0;
    if (power) {
        return 0; /* power condition is a no-op */
    }
    bool start = (cdb[4] & 0x01) != 0;
    bool loej = (cdb[4] & 0x02) != 0;
    if (!loej) {
        return 0; /* spin up/down is a no-op */
    }

    /* Virtual tray with real media state changes.  ImgBurn cycles the tray
       before verification so the host re-reads the TOC after finalization;
       a no-op here leaves its pre-burn layout cache in place. */
    if (!start) {
        if (s_scsi.prevent) {
            return fail(SENSE_ILLEGAL_REQUEST, ASC_MEDIUM_REMOVAL_PREVENTED, 0);
        }
        if (media_is_present()) {
            media_eject();
            scsi_notify_media_change(false);
        }
        return 0;
    }

    if (!media_is_present()) {
        if (!media_reload()) {
            return fail(SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0);
        }
        scsi_notify_media_change(true);
    }
    return 0;
}

static int32_t blank_command(const uint8_t *cdb)
{
    if (require_media() < 0) return -1;
    const media_status_t *m = media_get();
    if (!m->writable || m->write_protected) {
        return fail(SENSE_DATA_PROTECT, ASC_WRITE_PROTECTED, 0);
    }
    uint8_t type = cdb[1] & 0x07;
    if (type != 0 && type != 1 && type != 4) {
        return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
    }
    if (!media_blank()) {
        return fail(SENSE_MEDIUM_ERROR, 0x0C, 0x00);
    }
    return 0;
}

static int32_t close_track_session(const uint8_t *cdb)
{
    if (require_media() < 0) return -1;
    const media_status_t *m = media_get();
    if (!m->writable) {
        return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_COMMAND, 0);
    }
    uint8_t function = cdb[2] & 0x07;
    if (function == 0) {
        return 0;
    }
    if (function == 1) {
        (void)media_save_meta();
        return 0;
    }
    if (function == 2) {
        /* Close current session, keep the disc appendable. */
        if (!media_close_session(false)) {
            return fail(SENSE_MEDIUM_ERROR, 0x0C, 0x00);
        }
        return 0;
    }
    if (function == 3) {
        if (!media_close_session(true)) {
            return fail(SENSE_MEDIUM_ERROR, 0x0C, 0x00);
        }
        return 0;
    }
    return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
}

int32_t scsi_in_command(const uint8_t cdb[SCSI_CDB_MAX], uint8_t *buf, uint32_t cap)
{
    if (!begin_command(cdb[0])) {
        return -1;
    }
    switch (cdb[0]) {
    case 0xA4: { /* REPORT KEY */
        static const uint8_t rk[8] = {0x00, 0x06, 0x00, 0x00,
                                      0x25, 0xff, 0x01, 0x00};
        if (buf && cap >= sizeof(rk)) memcpy(buf, rk, sizeof(rk));
        return 8;
    }
    case OP_TEST_UNIT_READY:
        if (require_media() < 0) return -1;
        if (media_is_erasing()) return fail(SENSE_NOT_READY, 0x04, 0x07);
        if (media_is_formatting()) return fail(SENSE_NOT_READY, 0x04, 0x04);
        return 0;
    case OP_REQUEST_SENSE:
        return request_sense(buf);
    case OP_INQUIRY:
        return inquiry(cdb, buf, cap);
    case OP_MODE_SENSE_6:
        return mode_sense(cdb, buf, cap, false);
    case OP_MODE_SENSE_10:
        return mode_sense(cdb, buf, cap, true);
    case OP_READ_CAPACITY_10:
        return read_capacity(buf);
    case OP_READ_FORMAT_CAPACITIES:
        return read_format_capacities(buf);
    case OP_READ_DISC_INFORMATION:
        return read_disc_information(buf);
    case OP_READ_TRACK_INFORMATION:
        return read_track_information(cdb, buf);
    case OP_READ_TOC:
        return read_toc(cdb, buf);
    case OP_READ_HEADER: {
        if (require_media() < 0) return -1;
        uint32_t lba = util_be32(cdb + 2);
        const media_status_t *m = media_get();
        if (lba >= m->capacity_lba) {
            return fail(SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0);
        }
        memset(buf, 0, 8);
        buf[0] = 1;
        toc_address(buf + 4, (cdb[1] & 2) != 0, lba);
        return 8;
    }
    case OP_GET_CONFIGURATION:
        return get_configuration(cdb, buf, cap);
    case OP_GET_EVENT_STATUS:
        return get_event_status(cdb, buf);
    case OP_MECHANISM_STATUS:
        return mechanism_status(buf);
    case 0x04: /* FORMAT UNIT with no parameter list */
        return format_unit(cdb, NULL, 0);
    case OP_START_STOP_UNIT:
        return start_stop(cdb);
    case OP_PREVENT_ALLOW:
        s_scsi.prevent = (cdb[4] & 0x01) != 0;
        return 0;
    case OP_SYNCHRONIZE_CACHE_10:
        if (require_media() < 0) return -1;
        if (!media_sync()) {
            return fail(SENSE_MEDIUM_ERROR, 0x0C, 0x00);
        }
        return 0;
    case OP_BLANK:
        return blank_command(cdb);
    case OP_CLOSE_TRACK_SESSION:
        return close_track_session(cdb);
    case OP_RESERVE_TRACK: {
        if (require_media() < 0) return -1;
        const media_status_t *m = media_get();
        uint32_t reserved = util_be32(cdb + 5);
        if (!m->writable || m->write_protected) {
            return fail(SENSE_DATA_PROTECT, ASC_WRITE_PROTECTED, 0);
        }
        if (reserved && (uint64_t)m->data_lba + reserved > m->capacity_lba) {
            return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD, 0);
        }
        if (!media_reserve_track(m->data_lba, reserved)) {
            return fail(SENSE_MEDIUM_ERROR, 0x0C, 0x00);
        }
        return 0;
    }
    case OP_SEND_OPC:
        if (require_media() < 0) return -1;
        if (!media_get()->writable) {
            return fail(SENSE_DATA_PROTECT, ASC_WRITE_PROTECTED, 0);
        }
        return 0;
    case 0xA7: /* SET READ AHEAD */
        return 0;
    case 0xB6: /* SET STREAMING (parameter list handled as a no-op) */
        return 0;
    case OP_SET_CD_SPEED: {
        uint16_t read_speed = util_be16(cdb + 2);
        uint16_t write_speed = util_be16(cdb + 4);
        if (read_speed) {
            if (read_speed > 8 * 176) read_speed = 8 * 176;
            s_scsi.read_speed_kbs = read_speed;
        }
        if (write_speed) {
            if (write_speed > 4 * 176) write_speed = 4 * 176;
            s_scsi.write_speed_kbs = write_speed;
        }
        return 0;
    }
    case OP_READ_BUFFER_CAPACITY:
        memset(buf, 0, 12);
        /* Length of the following structure, including its reserved bytes. */
        util_put_be16(buf, 10);
        util_put_be32(buf + 4, 2048);
        util_put_be32(buf + 8, 2048);
        return 12;
    case OP_READ_BUFFER: {
        /* cdrecord queries the drive buffer before starting a write.  Mode 0
           returns a 4-byte buffer header; a zero size simply disables
           burn-free rather than aborting the burn. */
        uint32_t n = cap < 4 ? cap : 4;
        memset(buf, 0, n);
        return (int32_t)n;
    }
    case OP_READ_DISC_STRUCTURE: {
        if (require_media() < 0) return -1;
        uint32_t n = cap;
        if (n > 4096) n = 4096;
        if (n < 4) return (int32_t)n;
        memset(buf, 0, n);
        util_put_be16(buf, (uint16_t)(n - 2));
        return (int32_t)n;
    }
    case OP_GET_PERFORMANCE: {
        const media_status_t *m = media_get();
        uint8_t type = cdb[10];
        if (type == 0x03 && m->present) {
            /* Write Speed Descriptor: 4x CLV, 16-byte descriptor. */
            uint32_t len = 8 + 16;
            memset(buf, 0, len);
            util_put_be32(buf, 20); /* data length following this field */
            uint8_t *p = buf + 8;
            p[0] = 0x00; /* CLV, exact, no MRW */
            util_put_be32(p + 4, m->capacity_lba ? m->capacity_lba - 1 : 0);
            util_put_be32(p + 8, s_scsi.read_speed_kbs);
            util_put_be32(p + 12, s_scsi.write_speed_kbs);
            return (int32_t)len;
        }
        memset(buf, 0, 8);
        return 8;
    }
    case OP_READ_10:
    case OP_READ_12:
    case OP_READ_CD:
        return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_COMMAND, 0);
    case OP_WRITE_10:
    case OP_WRITE_12:
        return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_COMMAND, 0);
    case OP_VERIFY_10:
    case OP_VERIFY_12:
        if (require_media() < 0) return -1;
        return 0;
    default:
        return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_COMMAND, 0);
    }
}

int32_t scsi_out_command(const uint8_t cdb[SCSI_CDB_MAX], const uint8_t *data,
                         uint32_t len)
{
    if (!begin_command(cdb[0])) {
        return -1;
    }
    if (cdb[0] == 0x04) { /* FORMAT UNIT with parameter list */
        return format_unit(cdb, data, len);
    }
    if (cdb[0] == 0x5D) { /* SEND CUE SHEET: accepted, data is not needed for TAO/SAO emulation */
        (void)data;
        (void)len;
        LOGF("SCSI: cue sheet accepted (%lu bytes)", (unsigned long)len);
        return 0;
    }
    if (cdb[0] == 0xB6) { /* SET STREAMING */
        (void)data;
        (void)len;
        return 0;
    }
    if (cdb[0] == OP_MODE_SELECT_6 || cdb[0] == OP_MODE_SELECT_10) {
        uint32_t offset = cdb[0] == OP_MODE_SELECT_6 ? 4 : 8;
        while (offset + 2 <= len) {
            uint8_t page = data[offset] & 0x3F;
            uint8_t page_len = data[offset + 1];
            if (offset + 2 + page_len > len) {
                break;
            }
            if (page == 0x05 && page_len >= 5) {
                uint8_t *p = (uint8_t *)data + offset;
                s_scsi.write_type = p[2] & 0x0F;
                s_scsi.data_block_type = p[4];
            }
            offset += 2 + page_len;
        }
        return 0;
    }
    return fail(SENSE_ILLEGAL_REQUEST, ASC_INVALID_COMMAND, 0);
}
