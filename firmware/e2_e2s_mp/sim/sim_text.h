#pragma once
/* sim_text.h — host font-asset injection for the simulator.
 *
 * The text component does not read files on the host: text_font.c only gets
 * its HZK16 + GB2312 map through text_assets_set_host(), declared in the
 * *private* header components/text/text_internal.h under -DTEXT_HOST_BUILD
 * (PORT_SPEC §9.3 explicitly allows the simulator to include it). sim_text.c
 * is the only file in sim/ that does so.
 */
#include <stddef.h>
#include <stdint.h>

/* Result of sim_text_init(). Values are stable so run.sh can report them. */
#define SIM_TEXT_OK        0   /* HZK16 + map loaded, CJK glyphs available   */
#define SIM_TEXT_NO_HZ     1   /* map loaded, HZK16 missing/short -> hollow  */
#define SIM_TEXT_NO_MAP    2   /* HZK16 loaded, map missing -> hollow boxes  */
#define SIM_TEXT_NO_ASSETS 3   /* neither file usable                        */
#define SIM_TEXT_IO_ERROR  4   /* file exists but could not be read          */

/* Load HZK16_87.bin (fallback hz16_gb2312.bin) and gb2312_map.bin from
 * asset_dir, then call text_assets_set_host(). asset_dir == NULL falls back
 * to $MP_SIM_ASSETS, then to the path baked in by the Makefile.
 * Missing files are not fatal: every CJK glyph is then drawn as the text
 * component's 16 px hollow box. Returns one of SIM_TEXT_* above. */
int sim_text_init(const char *asset_dir);

/* 1 when both assets validated (tx_font_assets_ok()). */
int sim_text_assets_ok(void);

/* Diagnostics (valid after sim_text_init; never NULL). */
const char *sim_text_asset_dir(void);
const char *sim_text_hz_path(void);
const char *sim_text_map_path(void);
size_t sim_text_hz_len(void);
size_t sim_text_map_len(void);
