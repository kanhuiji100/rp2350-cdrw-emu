#ifndef CDRW_USB_CDROM_H
#define CDRW_USB_CDROM_H

#include <stdbool.h>
#include <stdint.h>

#define USB_CDROM_TRACE_DEPTH 16

typedef struct {
    uint8_t opcode;
    uint8_t dir_in;
    uint8_t status;     /* 0 = passed, 1 = failed, 0xFF = still pending */
    uint8_t cdb_len;
    uint32_t data_len;
    uint32_t lba;
    uint32_t blocks;
    uint32_t source_block_size;
    uint8_t key;
    uint8_t asc;
    uint8_t ascq;
    uint8_t discard;
} usb_cdrom_trace_t;

bool usb_cdrom_connected(void);
bool usb_cdrom_configured(void);
uint32_t usb_cdrom_last_activity_ms(void);

/* Called from the UI/SCSI layer after a host-visible media change has been
 * queued.  Kept tiny so it can be used while the USB stack is running. */
void usb_cdrom_kick(void);

/* Diagnostic SCSI command trace, index 0 = oldest retained entry. */
uint8_t usb_cdrom_trace_count(void);
bool usb_cdrom_trace_get(uint8_t index, usb_cdrom_trace_t *out);

#endif
