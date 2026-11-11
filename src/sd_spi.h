#ifndef CDRW_SD_SPI_H
#define CDRW_SD_SPI_H

#include <stdbool.h>
#include <stdint.h>

#define SD_BLOCK_SIZE 512u

bool sd_init(void);
bool sd_is_ready(void);
bool sd_card_present(void);
uint32_t sd_block_count(void);
void sd_fallback_read_clock(void);
bool sd_read_blocks(uint32_t lba, void *dst, uint32_t count);
bool sd_write_blocks(uint32_t lba, const void *src, uint32_t count);
bool sd_sync(void);

#endif
