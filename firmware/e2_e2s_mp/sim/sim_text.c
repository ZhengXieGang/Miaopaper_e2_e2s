/* sim_text.c — load the two font assets and hand them to the text component.
 *
 * On the device both files are linked in with EMBED_FILES and text_font.c
 * picks up the _binary_* symbols. On the host there are no such symbols, so
 * the simulator reads the same bytes from disk and calls the TEXT_HOST_BUILD
 * hook text_assets_set_host() (text_internal.h:104-108).
 *
 * Missing assets are a soft failure on purpose: text_font.c / mp_font.c both
 * fall back to a 16x16 hollow box for every CJK glyph, so the frame pipeline
 * still runs and the hole is visible in the PNG instead of being a crash.
 */
#include "sim_text.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text.h"
#include "text_internal.h"

#ifndef SIM_ASSET_DIR_DEFAULT
#define SIM_ASSET_DIR_DEFAULT "."
#endif

static uint8_t *s_hz;
static size_t s_hz_len;
static uint8_t *s_map;
static size_t s_map_len;

static char s_dir[512] = SIM_ASSET_DIR_DEFAULT;
static char s_hz_path[600];
static char s_map_path[600];
static int s_state = SIM_TEXT_NO_ASSETS;

static uint8_t *read_whole_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    size_t len = (size_t)n;
    uint8_t *buf = (uint8_t *)malloc(len ? len : 1u);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (len && fread(buf, 1, len, f) != len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = len;
    return buf;
}

static int join_path(char *dst, size_t cap, const char *dir, const char *name)
{
    int n = snprintf(dst, cap, "%s/%s", dir, name);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

/* Try name1 first, then name2 (NULL = none). Returns 1 on success. */
static int load_asset(char *path_out, size_t cap, char *name_out, size_t ncap,
                      const char *name1, const char *name2, uint8_t **buf_out,
                      size_t *len_out, size_t min_len)
{
    const char *names[2] = { name1, name2 };
    for (int i = 0; i < 2; i++) {
        if (!names[i]) {
            continue;
        }
        if (join_path(path_out, cap, s_dir, names[i]) != 0) {
            fprintf(stderr, "sim_assets: path too long: %s/%s\n", s_dir, names[i]);
            continue;
        }
        size_t len = 0;
        uint8_t *buf = read_whole_file(path_out, &len);
        if (!buf) {
            continue;
        }
        if (len < min_len) {
            fprintf(stderr,
                    "sim_assets: %s is %lu B, need at least %lu B; ignored\n",
                    path_out, (unsigned long)len, (unsigned long)min_len);
            free(buf);
            continue;
        }
        snprintf(name_out, ncap, "%s", names[i]);
        *buf_out = buf;
        *len_out = len;
        return 1;
    }
    path_out[0] = '\0';
    name_out[0] = '\0';
    return 0;
}

int sim_text_init(const char *asset_dir)
{
    const char *src = asset_dir;
    if (!src || !*src) {
        const char *env = getenv("MP_SIM_ASSETS");
        src = (env && *env) ? env : SIM_ASSET_DIR_DEFAULT;
    }
    int n = snprintf(s_dir, sizeof s_dir, "%s", src);
    if (n < 0 || (size_t)n >= sizeof s_dir) {
        fprintf(stderr, "sim_assets: asset dir path too long (max %u B)\n",
                (unsigned)(sizeof s_dir - 1));
        s_state = SIM_TEXT_IO_ERROR;
        return s_state;
    }
    /* Drop any trailing slash so the printed paths stay tidy. */
    size_t dlen = strlen(s_dir);
    while (dlen > 1 && s_dir[dlen - 1] == '/') {
        s_dir[--dlen] = '\0';
    }

    char hz_name[64] = "";
    char map_name[64] = "";
    int hz_ok = load_asset(s_hz_path, sizeof s_hz_path, hz_name, sizeof hz_name,
                           "HZK16_87.bin", "hz16_gb2312.bin",
                           &s_hz, &s_hz_len, (size_t)TEXT_HZ16_BYTES);
    int map_ok = load_asset(s_map_path, sizeof s_map_path, map_name,
                            sizeof map_name, "gb2312_map.bin", NULL,
                            &s_map, &s_map_len, (size_t)TEXT_MAP_HEADER_BYTES);

    fprintf(stderr, "sim_assets: dir %s\n", s_dir);
    if (hz_ok) {
        fprintf(stderr, "sim_assets: %s %lu B (want %u)\n", hz_name,
                (unsigned long)s_hz_len, (unsigned)TEXT_HZ16_BYTES);
        if (s_hz_len != (size_t)TEXT_HZ16_BYTES) {
            fprintf(stderr,
                    "sim_assets: HZK16 size != %u B; GB2312 lookup may fail\n",
                    (unsigned)TEXT_HZ16_BYTES);
        }
    } else {
        fprintf(stderr,
                "sim_assets: %s/HZK16_87.bin (or hz16_gb2312.bin) missing; "
                "CJK will be hollow boxes\n", s_dir);
    }
    if (map_ok) {
        fprintf(stderr, "sim_assets: %s %lu B (want %u)\n", map_name,
                (unsigned long)s_map_len, (unsigned)TEXT_MAP_BYTES);
    } else {
        fprintf(stderr,
                "sim_assets: %s/gb2312_map.bin missing; CJK will be hollow "
                "boxes\n", s_dir);
    }

    /* Always call the hook, even with NULLs: text_assets_set_host() marks the
     * lazy init as done and logs the degraded state once. */
    text_assets_set_host(s_hz, s_hz_len, s_map, s_map_len);

    if (hz_ok && map_ok) {
        s_state = tx_font_assets_ok() ? SIM_TEXT_OK : SIM_TEXT_IO_ERROR;
    } else if (map_ok) {
        s_state = SIM_TEXT_NO_HZ;
    } else if (hz_ok) {
        s_state = SIM_TEXT_NO_MAP;
    } else {
        s_state = SIM_TEXT_NO_ASSETS;
    }

    if (s_state == SIM_TEXT_OK) {
        fprintf(stderr, "sim_assets: OK (CJK glyphs available)\n");
    } else if (s_state == SIM_TEXT_IO_ERROR) {
        fprintf(stderr,
                "sim_assets: assets present but rejected by text_font.c "
                "(bad CRC/size?); CJK will be hollow boxes\n");
    }
    return s_state;
}

int sim_text_assets_ok(void)
{
    return s_state == SIM_TEXT_OK;
}

const char *sim_text_asset_dir(void) { return s_dir; }
const char *sim_text_hz_path(void) { return s_hz_path; }
const char *sim_text_map_path(void) { return s_map_path; }
size_t sim_text_hz_len(void) { return s_hz_len; }
size_t sim_text_map_len(void) { return s_map_len; }
