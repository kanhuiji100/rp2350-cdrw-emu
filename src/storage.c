#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "debug_log.h"
#include "ff.h"
#include "sd_spi.h"
#include "util.h"

#define DISC_DIR "/discs"
#define CONFIG_FILE "/cdrw.cfg"

static FATFS s_fs;
static bool s_mounted;
static bool s_ready;
static storage_error_t s_last_error = STORAGE_ERR_SD_INIT;

static int compare_discs(const void *a, const void *b)
{
    const disc_entry_t *da = (const disc_entry_t *)a;
    const disc_entry_t *db = (const disc_entry_t *)b;
    return strcasecmp(da->name, db->name);
}

bool storage_init(void)
{
    s_mounted = false;
    s_ready = false;
    s_last_error = STORAGE_ERR_SD_INIT;

    if (!sd_init()) {
        LOGF("storage: sd_init failed");
        return false;
    }
    LOGF("storage: SD initialized, mounting FatFs");
    s_last_error = STORAGE_ERR_FAT_MOUNT;
    FRESULT fr = f_mount(&s_fs, "", 1);
    if (fr != FR_OK) {
        LOGF("storage: f_mount failed FRESULT=%d", (int)fr);
#if CDRW_SD_READ_HZ > CDRW_SD_SPI_HZ
        LOGF("storage: retrying mount at safe read clock");
        sd_fallback_read_clock();
        if (!sd_init()) {
            LOGF("storage: sd reinit after clock fallback failed");
            return false;
        }
        fr = f_mount(&s_fs, "", 1);
        if (fr != FR_OK) {
            LOGF("storage: f_mount retry failed FRESULT=%d", (int)fr);
            return false;
        }
#else
        return false;
#endif
    }
    s_mounted = true;
    s_ready = true;
    s_last_error = STORAGE_ERR_NONE;
    LOGF("storage: FatFs mounted, %lu bytes free",
         (unsigned long)storage_free_bytes());

    storage_make_disc_dir();
    return true;
}

bool storage_ready(void)
{
    return s_ready;
}

storage_error_t storage_last_error(void)
{
    return s_last_error;
}

void storage_deinit(void)
{
    if (s_mounted) {
        f_mount(NULL, "", 0);
        s_mounted = false;
    }
    s_ready = false;
    s_last_error = STORAGE_ERR_SD_INIT;
}

static void add_disc(disc_entry_t *out, size_t max, size_t *count,
                     const char *path, const char *name)
{
    if (*count >= max) {
        return;
    }
    if (!util_has_suffix_ci(path, ".iso") && !util_has_suffix_ci(path, ".cdr")) {
        return;
    }
    disc_entry_t *e = &out[*count];
    memset(e, 0, sizeof(*e));
    util_safe_copy(e->path, sizeof(e->path), path);
    util_safe_copy(e->name, sizeof(e->name), name);
    e->format = util_has_suffix_ci(path, ".iso") ? DISC_FORMAT_ISO : DISC_FORMAT_CDRW;

    FILINFO fi;
    if (f_stat(path, &fi) == FR_OK) {
        e->sectors = (uint32_t)(fi.fsize / 2048u);
    }
    (*count)++;
}

static void scan_dir(const char *dir, disc_entry_t *out, size_t max, size_t *count)
{
    DIR d;
    FILINFO fi;
    if (f_opendir(&d, dir) != FR_OK) {
        return;
    }
    while (*count < max && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (fi.fattrib & AM_DIR) {
            continue;
        }
        char full[STORAGE_PATH_MAX];
        if (strcmp(dir, "/") == 0) {
            snprintf(full, sizeof(full), "/%s", fi.fname);
        } else {
            snprintf(full, sizeof(full), "%s/%s", dir, fi.fname);
        }
        if ((fi.fattrib & AM_HID) || fi.fname[0] == '.') {
            continue;
        }
        add_disc(out, max, count, full, fi.fname);
    }
    f_closedir(&d);
}

size_t storage_scan_discs(disc_entry_t *out, size_t max_entries)
{
    if (!s_ready || !out || !max_entries) {
        return 0;
    }
    size_t count = 0;
    scan_dir("/", out, max_entries, &count);
    scan_dir(DISC_DIR, out, max_entries, &count);
    if (count > 1) {
        qsort(out, count, sizeof(disc_entry_t), compare_discs);
    }
    return count;
}

bool storage_file_exists(const char *path)
{
    FILINFO fi;
    return s_ready && f_stat(path, &fi) == FR_OK;
}

uint32_t storage_file_sectors(const char *path)
{
    FILINFO fi;
    if (!s_ready || f_stat(path, &fi) != FR_OK) {
        return 0;
    }
    return (uint32_t)(fi.fsize / 2048u);
}

uint64_t storage_free_bytes(void)
{
    if (!s_ready) {
        return 0;
    }
    DWORD free_clusters = 0;
    FATFS *fs = NULL;
    FRESULT fr = f_getfree("", &free_clusters, &fs);
    if (fr != FR_OK || !fs) {
        return 0;
    }
    /* Some cards have a stale FSInfo free count of zero.  Force one FAT scan
       so the UI/tooling does not incorrectly think the volume is full. */
    if (free_clusters == 0) {
        fs->free_clst = 0xFFFFFFFFu;
        fr = f_getfree("", &free_clusters, &fs);
        if (fr != FR_OK || !fs) {
            return 0;
        }
    }
    return (uint64_t)free_clusters * fs->csize * 512ull;
}

bool storage_make_disc_dir(void)
{
    if (!s_ready) {
        return false;
    }
    FRESULT fr = f_mkdir(DISC_DIR);
    return fr == FR_OK || fr == FR_EXIST;
}

static uint8_t s_zero_chunk[4096];

static bool zero_range(FIL *f, uint64_t off, uint64_t len)
{
    while (len) {
        UINT chunk = len > sizeof(s_zero_chunk) ? sizeof(s_zero_chunk) : (UINT)len;
        UINT bw = 0;
        if (f_lseek(f, (FSIZE_t)off) != FR_OK) {
            return false;
        }
        if (f_write(f, s_zero_chunk, chunk, &bw) != FR_OK || bw != chunk) {
            return false;
        }
        off += chunk;
        len -= chunk;
    }
    return true;
}

bool storage_zero_file_edges(FIL *f, uint64_t bytes)
{
    if (!f || bytes == 0) {
        return false;
    }
    const uint64_t edge = 1024ull * 1024ull;
    bool ok;
    if (bytes <= edge * 2ull) {
        ok = zero_range(f, 0, bytes);
    } else {
        ok = zero_range(f, 0, edge) &&
             zero_range(f, bytes - edge, edge);
    }
    return ok && f_sync(f) == FR_OK;
}

bool storage_create_blank_image(const char *path, uint32_t sectors)
{
    if (!s_ready || !path || !sectors) {
        return false;
    }
    uint64_t bytes = (uint64_t)sectors * 2048ull;
    if (bytes > 0xFFFFFFFFull) {
        return false; /* FatFs is configured for 32-bit files (FAT32). */
    }
    if (storage_free_bytes() < bytes + (16ull * 1024ull * 1024ull)) {
        return false;
    }

    FIL f;
    FRESULT fr = f_open(&f, path, FA_CREATE_ALWAYS | FA_READ | FA_WRITE);
    if (fr != FR_OK) {
        return false;
    }

    fr = f_expand(&f, (FSIZE_t)bytes, 0);
    if (fr != FR_OK || !storage_zero_file_edges(&f, bytes)) {
        f_close(&f);
        f_unlink(path);
        return false;
    }
    f_close(&f);
    return true;
}

bool storage_save_last_disc(const char *path)
{
    if (!s_ready || !path) {
        return false;
    }
    FIL f;
    if (f_open(&f, CONFIG_FILE, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        return false;
    }
    UINT bw = 0;
    UINT len = (UINT)strlen(path);
    FRESULT fr = f_write(&f, path, len, &bw);
    f_close(&f);
    return fr == FR_OK && bw == len;
}

bool storage_load_last_disc(char *path, size_t path_size)
{
    if (!s_ready || !path || path_size < 2) {
        return false;
    }
    FIL f;
    if (f_open(&f, CONFIG_FILE, FA_READ) != FR_OK) {
        return false;
    }
    char buf[STORAGE_PATH_MAX];
    UINT br = 0;
    FRESULT fr = f_read(&f, buf, sizeof(buf) - 1, &br);
    f_close(&f);
    if (fr != FR_OK || br == 0) {
        return false;
    }
    buf[br] = '\0';
    char *nl = strpbrk(buf, "\r\n");
    if (nl) *nl = '\0';
    if (buf[0] == '\0' || !storage_file_exists(buf)) {
        return false;
    }
    util_safe_copy(path, path_size, buf);
    return true;
}

bool storage_new_disc_path(char *path, size_t path_size)
{
    if (!s_ready || !path) {
        return false;
    }
    storage_make_disc_dir();
    for (unsigned i = 1; i <= 99; i++) {
        char candidate[STORAGE_PATH_MAX];
        snprintf(candidate, sizeof(candidate), "%s/DISC%02u.CDR", DISC_DIR, i);
        if (!storage_file_exists(candidate)) {
            util_safe_copy(path, path_size, candidate);
            return true;
        }
    }
    return false;
}
