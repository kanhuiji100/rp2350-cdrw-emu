/*
 * USB descriptors for a bus-powered, full-speed (USB 1.1) CD-RW drive.
 * The interface uses the Mass Storage Bulk-Only Transport, but the class
 * driver in usb_cdrom.c reports SCSI PDT 5, so hosts load their optical
 * driver rather than usb-storage's direct-access disk path.
 */
#include <string.h>

#include "tusb.h"
#include "class/msc/msc.h"
#include "pico/unique_id.h"

#define USB_VID 0x1209
#define USB_PID 0xCD01
#define USB_BCD 0x0110

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = USB_BCD,
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)
#define EPNUM_MSC_OUT 0x01
#define EPNUM_MSC_IN 0x81

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN, 0x80, 250),
    TUD_MSC_DESCRIPTOR(0, 0, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

static uint16_t s_string_desc[32];

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&desc_device;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

static uint16_t make_string(const char *s)
{
    uint16_t len = (uint16_t)strlen(s);
    if (len > 31) len = 31;
    s_string_desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 + 2 * len));
    for (uint16_t i = 0; i < len; i++) {
        s_string_desc[1 + i] = (uint8_t)s[i];
    }
    return (uint16_t)(2 + 2 * len);
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    if (index == 0) {
        static const uint16_t languages[] = {0x0409};
        s_string_desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | 4);
        s_string_desc[1] = languages[0];
        return s_string_desc;
    }
    switch (index) {
    case 1:
        (void)make_string("RP2350 CD-RW Project");
        break;
    case 2:
        (void)make_string("RP2350 CD-RW Drive");
        break;
    case 3: {
        char id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];
        pico_get_unique_board_id_string(id, sizeof(id));
        (void)make_string(id);
        break;
    }
    default:
        return NULL;
    }
    return s_string_desc;
}
