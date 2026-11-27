/* mp_toc.c — 电子书自动章节识别与 SD 定长索引缓存实现。
 *
 * 格式（全小端序，仅写 /sdcard/mp/toc<slot>.bin）：
 *   头部 16 B：
 *     [0..3]   "MTOC"
 *     [4]      ver = 1
 *     [5]      enc (MP_ENC_ASCII / MP_ENC_GB2312)
 *     [6..7]   count (uint16_t, 0..MP_TOC_MAX)
 *     [8..11]  book_len (uint32_t)
 *     [12..15] book_crc (uint32_t，书文件前 min(book_len, 4096) B 的 IEEE CRC-32)
 *   条目 N × 36 B：
 *     [0..3]   offset (uint32_t)
 *     [4..35]  title[32] (NUL 结尾，GB2312 绝不截断半汉字)
 */
#include "mp_toc.h"

#include <string.h>

#ifndef MP_TOC_HOST_ONLY
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"

#include "mp_store.h"
#include "rc_common.h"
#else
#ifndef MP_ENC_ASCII
#define MP_ENC_ASCII 0
#endif
#ifndef MP_ENC_GB2312
#define MP_ENC_GB2312 1
#endif
#endif

/* ------------------------------------------------------------------ *
 * 章节标题行匹配（纯 C，无 I/O，主机与固件共用）
 * ------------------------------------------------------------------ */

#define TOC_MAX_LEAD_SKIP 16u     /* 最多允许跳过的前导空白字节数 */
#define TOC_MAX_EFF_BYTES 76u     /* 有效标题最大字节数（约 38 汉字，滤掉正文段落） */
#define TOC_LINE_BUF_BYTES 96u    /* 扫描期单行静态缓冲上限 */

static bool is_ascii_space(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

static bool is_ascii_digit(uint8_t c)
{
    return c >= '0' && c <= '9';
}

static uint8_t to_lower_ascii(uint8_t c)
{
    return (c >= 'A' && c <= 'Z') ? (uint8_t)(c + ('a' - 'A')) : c;
}

static bool is_gb_full_space(const uint8_t *p, size_t rem)
{
    return rem >= 2u && p[0] == 0xA1u && p[1] == 0xA1u;
}

static bool match_gb2(const uint8_t *p, size_t rem, uint8_t b0, uint8_t b1)
{
    return rem >= 2u && p[0] == b0 && p[1] == b1;
}

/* 判断双字节是否为中文数字（GB2312 十五字 + GBK 〇 + 全角数字 ０..９）。 */
static bool is_gb_num(uint8_t hi, uint8_t lo)
{
    if (hi == 0xA3u && lo >= 0xB0u && lo <= 0xB9u) {
        return true;              /* 全角数字 ０..９ */
    }
    if (hi == 0xA9u && lo == 0x96u) {
        return true;              /* GBK 圆零 〇 */
    }
    /* 零一二三四五六七八九十百千万两（GB2312 精确编码） */
    static const uint8_t k_cn_digits[][2] = {
        {0xC1u, 0xE3u}, /* 零 */
        {0xD2u, 0xBBu}, /* 一 */
        {0xB6u, 0xFEu}, /* 二 */
        {0xC8u, 0xFDu}, /* 三 */
        {0xCBu, 0xC4u}, /* 四 */
        {0xCEu, 0xE5u}, /* 五 */
        {0xC1u, 0xF9u}, /* 六 */
        {0xC6u, 0xDFu}, /* 七 */
        {0xB0u, 0xCBu}, /* 八 */
        {0xBEu, 0xC5u}, /* 九 */
        {0xCAu, 0xAEu}, /* 十 */
        {0xB0u, 0xD9u}, /* 百 */
        {0xC7u, 0xA7u}, /* 千 */
        {0xCDu, 0xF2u}, /* 万 */
        {0xC1u, 0xBDu}, /* 两 */
    };
    for (size_t i = 0; i < sizeof(k_cn_digits) / sizeof(k_cn_digits[0]); i++) {
        if (k_cn_digits[i][0] == hi && k_cn_digits[i][1] == lo) {
            return true;
        }
    }
    return false;
}

/* 判断双字节是否为章节量词（章/回/节/卷/部/篇/集/幕）。 */
static bool is_gb_unit(uint8_t hi, uint8_t lo)
{
    static const uint8_t k_units[][2] = {
        {0xD5u, 0xC2u}, /* 章 */
        {0xBBu, 0xD8u}, /* 回 */
        {0xBDu, 0xDAu}, /* 节 */
        {0xBEu, 0xEDu}, /* 卷 */
        {0xB2u, 0xBFu}, /* 部 */
        {0xC6u, 0xAAu}, /* 篇 */
        {0xBCu, 0xAFu}, /* 集 */
        {0xC4u, 0xBBu}, /* 幕 */
    };
    for (size_t i = 0; i < sizeof(k_units) / sizeof(k_units[0]); i++) {
        if (k_units[i][0] == hi && k_units[i][1] == lo) {
            return true;
        }
    }
    return false;
}

/* 判断双字节是否为中文标题常见分隔符（全角空格/冒号/顿号/左括号/方头括号/破折号/间隔号）。 */
static bool is_gb_delim(const uint8_t *p, size_t rem)
{
    if (rem == 0) {
        return true;
    }
    uint8_t c = p[0];
    if (c == ' ' || c == '\t' || c == ':' || c == '.' || c == '-' ||
        c == '_' || c == ',' || c == '(' || c == '[' || c == '/') {
        return true;
    }
    if (rem < 2u) {
        return false;
    }
    uint8_t b1 = p[1];
    if (c == 0xA1u && (b1 == 0xA1u || b1 == 0xA2u || b1 == 0xA4u ||
                       b1 == 0xAAu || b1 == 0xBEu)) {
        return true;              /* 　 、 · — 【 */
    }
    if (c == 0xA3u && (b1 == 0xBAu || b1 == 0xA8u || b1 == 0xACu)) {
        return true;              /* ： （ ， */
    }
    return false;
}

/* 判断行内是否含明显正文特征（左引号或长句末尾句号），降低误报率。 */
static bool looks_like_cn_sentence(const uint8_t *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        if (s[i] >= 0xA1u && i + 1u < n) {
            /* 出现中文双引号“/”通常是对话正文 */
            if (s[i] == 0xA1u && (s[i + 1u] == 0xB0u || s[i + 1u] == 0xB1u)) {
                return true;
            }
            i += 2u;
        } else {
            if (s[i] == 0x22u) {
                return true;
            }
            i += 1u;
        }
    }
    /* 末尾为中文句号“。”且长度偏长，多为正文陈述句 */
    if (n >= 22u && s[n - 2u] == 0xA1u && s[n - 1u] == 0xA3u) {
        return true;
    }
    return false;
}

/* 消费 1..max_cnt 个中文/阿拉伯/全角数字，返回消费的字节数和字符数。 */
static size_t consume_cn_number(const uint8_t *s, size_t rem,
                                unsigned max_cnt, unsigned *out_cnt)
{
    size_t i = 0;
    unsigned cnt = 0;
    while (i < rem && cnt < max_cnt) {
        if (is_ascii_digit(s[i])) {
            i += 1u;
            cnt++;
        } else if (i + 1u < rem && is_gb_num(s[i], s[i + 1u])) {
            i += 2u;
            cnt++;
        } else {
            break;
        }
    }
    if (out_cnt) {
        *out_cnt = cnt;
    }
    return i;
}

/* 跳过最多 2 个半角或全角空格。 */
static size_t skip_opt_spaces(const uint8_t *s, size_t rem)
{
    size_t i = 0;
    unsigned n = 0;
    while (i < rem && n < 2u) {
        if (s[i] == ' ' || s[i] == '\t') {
            i += 1u;
            n++;
        } else if (is_gb_full_space(s + i, rem - i)) {
            i += 2u;
            n++;
        } else {
            break;
        }
    }
    return i;
}

/* 匹配 GB2312 中文标准章名。 */
static bool match_gb2312_heading(const uint8_t *s, size_t n)
{
    if (n < 4u || looks_like_cn_sentence(s, n)) {
        return false;
    }

    /* 规则 A：第 + 数字(1..12) + 量词(章/回/节/卷/部/篇/集/幕) */
    if (match_gb2(s, n, 0xB5u, 0xDAu)) {
        size_t pos = 2u;
        pos += skip_opt_spaces(s + pos, n - pos);
        unsigned num_cnt = 0;
        pos += consume_cn_number(s + pos, n - pos, 12u, &num_cnt);
        if (num_cnt > 0) {
            pos += skip_opt_spaces(s + pos, n - pos);
            if (pos + 2u <= n && is_gb_unit(s[pos], s[pos + 1u])) {
                pos += 2u;
                if (pos == n || is_gb_delim(s + pos, n - pos)) {
                    return true;
                }
                /* 允许无空格紧跟短标题（如“第一章初遇”），过滤长正文句。 */
                if (n <= 44u &&
                    !(s[n - 2u] == 0xA1u && s[n - 1u] == 0xA3u)) {
                    return true;
                }
            }
        }
    }

    /* 规则 B：卷 + 数字(1..8) + (行尾或分隔符) */
    if (match_gb2(s, n, 0xBEu, 0xEDu)) {
        size_t pos = 2u;
        pos += skip_opt_spaces(s + pos, n - pos);
        unsigned num_cnt = 0;
        pos += consume_cn_number(s + pos, n - pos, 8u, &num_cnt);
        if (num_cnt > 0 && (pos == n || is_gb_delim(s + pos, n - pos))) {
            return true;
        }
    }

    /* 规则 C：十二个常见特殊独立章名（后接行尾、分隔符或数字） */
    static const uint8_t k_special[][4] = {
        {0xD0u, 0xF2u, 0xD5u, 0xC2u}, /* 序章 */
        {0xD0u, 0xF2u, 0xD1u, 0xD4u}, /* 序言 */
        {0xC7u, 0xB0u, 0xD1u, 0xD4u}, /* 前言 */
        {0xD2u, 0xFDu, 0xD1u, 0xD4u}, /* 引言 */
        {0xD0u, 0xA8u, 0xD7u, 0xD3u}, /* 楔子 */
        {0xCEu, 0xB2u, 0xC9u, 0xF9u}, /* 尾声 */
        {0xBAu, 0xF3u, 0xBCu, 0xC7u}, /* 后记 */
        {0xB7u, 0xACu, 0xCDu, 0xE2u}, /* 番外 */
        {0xD6u, 0xD5u, 0xD5u, 0xC2u}, /* 终章 */
        {0xD0u, 0xF2u, 0xC4u, 0xBBu}, /* 序幕 */
        {0xD2u, 0xFDu, 0xD7u, 0xD3u}, /* 引子 */
        {0xB8u, 0xBDu, 0xD1u, 0xD4u}, /* 附言 */
    };
    for (size_t i = 0; i < sizeof(k_special) / sizeof(k_special[0]); i++) {
        if (memcmp(s, k_special[i], 4u) == 0) {
            if (n == 4u || is_gb_delim(s + 4u, n - 4u)) {
                return true;
            }
            /* 允许“番外一”“序章2”等紧接数字的写法 */
            unsigned num_cnt = 0;
            size_t num_bytes = consume_cn_number(s + 4u, n - 4u, 6u, &num_cnt);
            if (num_cnt > 0 &&
                (4u + num_bytes == n ||
                 is_gb_delim(s + 4u + num_bytes, n - 4u - num_bytes))) {
                return true;
            }
            break;
        }
    }

    return false;
}

static bool match_ascii_ci(const uint8_t *s, size_t n, const char *kw,
                           size_t kw_len)
{
    if (n < kw_len) {
        return false;
    }
    for (size_t i = 0; i < kw_len; i++) {
        if (to_lower_ascii(s[i]) != (uint8_t)kw[i]) {
            return false;
        }
    }
    return true;
}

static bool is_roman_char(uint8_t c)
{
    uint8_t l = to_lower_ascii(c);
    return l == 'i' || l == 'v' || l == 'x' || l == 'l' || l == 'c' ||
           l == 'd' || l == 'm';
}

static bool is_ascii_delim(uint8_t c)
{
    return c == ' ' || c == '\t' || c == ':' || c == '.' || c == '-' ||
           c == '_' || c == ',' || c == '(' || c == '[' || c == '/';
}

/* 匹配英文标准章名（Chapter / Part / Book / Section / Prologue 等）。 */
static bool match_ascii_heading(const uint8_t *s, size_t n)
{
    static const char *const k_numbered[] = {
        "chapter", "part", "book", "section",
    };
    for (size_t k = 0; k < sizeof(k_numbered) / sizeof(k_numbered[0]); k++) {
        size_t kw_len = strlen(k_numbered[k]);
        if (!match_ascii_ci(s, n, k_numbered[k], kw_len)) {
            continue;
        }
        size_t pos = kw_len;
        if (pos >= n || !(s[pos] == ' ' || s[pos] == '\t' || s[pos] == '.' ||
                          s[pos] == '-')) {
            continue;
        }
        while (pos < n && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '.' ||
                           s[pos] == '-')) {
            pos++;
        }
        if (pos >= n) {
            continue;
        }
        /* 后接阿拉伯数字或罗马数字 */
        size_t num_start = pos;
        if (is_ascii_digit(s[pos])) {
            while (pos < n && is_ascii_digit(s[pos])) {
                pos++;
            }
        } else if (is_roman_char(s[pos])) {
            while (pos < n && is_roman_char(s[pos])) {
                pos++;
            }
        } else {
            continue;
        }
        if (pos > num_start && (pos - num_start) <= 10u &&
            (pos == n || is_ascii_delim(s[pos]))) {
            return true;
        }
    }

    static const char *const k_standalone[] = {
        "prologue", "epilogue", "preface", "foreword", "afterword",
        "introduction",
    };
    for (size_t k = 0; k < sizeof(k_standalone) / sizeof(k_standalone[0]); k++) {
        size_t kw_len = strlen(k_standalone[k]);
        if (match_ascii_ci(s, n, k_standalone[k], kw_len)) {
            if (n == kw_len || is_ascii_delim(s[kw_len])) {
                return true;
            }
        }
    }

    return false;
}

/* 将有效标题复制到 32 B 目标缓冲，GB2312 按双字节边界截断，绝不切碎半汉字。 */
static void copy_title_safe(const uint8_t *s, size_t n, uint8_t enc,
                            char out[MP_TOC_TITLE_BYTES])
{
    const size_t max_bytes = MP_TOC_TITLE_BYTES - 1u;
    size_t dst = 0;
    size_t i = 0;

    while (i < n && dst < max_bytes) {
        uint8_t c = s[i];
        if (enc == MP_ENC_GB2312 && c >= 0xA1u && i + 1u < n) {
            if (dst + 2u > max_bytes) {
                break;
            }
            /* 将标题内连续全角空格规范为半角空格，节省窄屏显示宽度 */
            if (c == 0xA1u && s[i + 1u] == 0xA1u) {
                if (dst > 0 && out[dst - 1u] != ' ') {
                    out[dst++] = ' ';
                }
            } else {
                out[dst++] = (char)c;
                out[dst++] = (char)s[i + 1u];
            }
            i += 2u;
        } else {
            char ch = (c == '\t' || c == '\r') ? ' ' : (char)c;
            if (ch == ' ' && dst > 0 && out[dst - 1u] == ' ') {
                i += 1u;
                continue;
            }
            out[dst++] = ch;
            i += 1u;
        }
    }
    while (dst > 0 && out[dst - 1u] == ' ') {
        dst--;
    }
    out[dst] = '\0';
}

bool mp_toc_match_line(const uint8_t *line, size_t len, uint8_t enc,
                       char out_title[MP_TOC_TITLE_BYTES],
                       uint32_t *out_lead_skip)
{
    if (!line || len == 0 || !out_title) {
        return false;
    }

    /* 1) 跳过前导半角/全角空白（超过上限视作非标题缩进）。 */
    size_t start = 0;
    while (start < len && start < TOC_MAX_LEAD_SKIP) {
        if (is_ascii_space(line[start])) {
            start += 1u;
        } else if (enc == MP_ENC_GB2312 &&
                   is_gb_full_space(line + start, len - start)) {
            if (start + 2u > TOC_MAX_LEAD_SKIP) {
                break;
            }
            start += 2u;
        } else {
            break;
        }
    }

    /* 2) 去除尾随半角/全角空白。 */
    size_t end = len;
    while (end > start) {
        if (is_ascii_space(line[end - 1u])) {
            end -= 1u;
        } else if (enc == MP_ENC_GB2312 && end >= start + 2u &&
                   line[end - 2u] == 0xA1u && line[end - 1u] == 0xA1u) {
            end -= 2u;
        } else {
            break;
        }
    }

    if (end <= start) {
        return false;
    }
    size_t eff = end - start;
    if (eff < 2u || eff > TOC_MAX_EFF_BYTES) {
        return false;
    }

    const uint8_t *s = line + start;
    bool hit = false;
    if (enc == MP_ENC_GB2312) {
        hit = match_gb2312_heading(s, eff) || match_ascii_heading(s, eff);
    } else {
        hit = match_ascii_heading(s, eff);
    }
    if (!hit) {
        return false;
    }

    copy_title_safe(s, eff, enc, out_title);
    if (out_title[0] == '\0') {
        return false;
    }
    if (out_lead_skip) {
        *out_lead_skip = (uint32_t)start;
    }
    return true;
}

/* ------------------------------------------------------------------ *
 * SD 索引缓存（仅编译进 ESP-IDF 固件；仅操作 /sdcard/mp/）
 * ------------------------------------------------------------------ */
#ifndef MP_TOC_HOST_ONLY

static const char *TAG = "mp_toc";

#ifndef MP_TOC_DIR
#define MP_TOC_DIR "/sdcard/mp"
#endif
#define MP_TOC_BOOK_FMT MP_TOC_DIR "/b%u.bin"
#define MP_TOC_FILE_FMT MP_TOC_DIR "/toc%u.bin"
#define MP_TOC_TMP_FMT  MP_TOC_DIR "/toc%u.tmp"

#define TOC_HDR_BYTES 16u
#define TOC_ENTRY_BYTES 36u
#define TOC_VER 1u
#define TOC_CRC_SAMPLE_BYTES 4096u
#define TOC_WR_BATCH 32u

/* 静态扫描与批量写缓冲：单任务调用，不占用主任务 8 KiB 栈。 */
static uint8_t s_scan_buf[2048];
static uint8_t s_wr_buf[TOC_WR_BATCH * TOC_ENTRY_BYTES];

static void wr_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int write_all_fd(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) {
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_all_fd(int fd, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n <= 0) {
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void toc_path(uint16_t slot, char *out, size_t cap)
{
    snprintf(out, cap, MP_TOC_FILE_FMT, (unsigned)slot);
}

static void toc_tmp_path(uint16_t slot, char *out, size_t cap)
{
    snprintf(out, cap, MP_TOC_TMP_FMT, (unsigned)slot);
}

static void book_path(uint16_t slot, char *out, size_t cap)
{
    snprintf(out, cap, MP_TOC_BOOK_FMT, (unsigned)slot);
}

static void encode_hdr(uint8_t hdr[TOC_HDR_BYTES], uint8_t enc, uint16_t count,
                       uint32_t book_len, uint32_t book_crc)
{
    hdr[0] = 'M';
    hdr[1] = 'T';
    hdr[2] = 'O';
    hdr[3] = 'C';
    hdr[4] = TOC_VER;
    hdr[5] = enc;
    wr_le16(&hdr[6], count);
    wr_le32(&hdr[8], book_len);
    wr_le32(&hdr[12], book_crc);
}

/* 计算书文件前 4 KiB 的 CRC-32，用于识别同槽位换书。 */
static int calc_book_head_crc(int bfd, uint32_t book_len, uint32_t *out_crc)
{
    if (lseek(bfd, 0, SEEK_SET) < 0) {
        return -1;
    }
    uint32_t rem = book_len < TOC_CRC_SAMPLE_BYTES ? book_len
                                                   : TOC_CRC_SAMPLE_BYTES;
    uint32_t crc = 0;
    while (rem > 0) {
        size_t chunk = rem > sizeof(s_scan_buf) ? sizeof(s_scan_buf)
                                                : (size_t)rem;
        ssize_t n = read(bfd, s_scan_buf, chunk);
        if (n <= 0) {
            return -1;
        }
        crc = rc_crc32_update(crc, s_scan_buf, (size_t)n);
        rem -= (uint32_t)n;
    }
    *out_crc = crc;
    return 0;
}

/* 检查既有索引缓存是否匹配当前书文件。命中返回 1，未命中返回 0。 */
static int check_cache_valid(const char *path, uint8_t enc, uint32_t book_len,
                             uint32_t book_crc, uint16_t *out_count)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    uint8_t hdr[TOC_HDR_BYTES];
    struct stat st;
    int hit = 0;
    if (fstat(fd, &st) == 0 && st.st_size >= (off_t)TOC_HDR_BYTES &&
        read_all_fd(fd, hdr, sizeof(hdr)) == 0) {
        uint16_t cnt = rd_le16(&hdr[6]);
        uint32_t flen = rd_le32(&hdr[8]);
        uint32_t fcrc = rd_le32(&hdr[12]);
        off_t expect = (off_t)TOC_HDR_BYTES +
                       (off_t)cnt * (off_t)TOC_ENTRY_BYTES;
        if (hdr[0] == 'M' && hdr[1] == 'T' && hdr[2] == 'O' && hdr[3] == 'C' &&
            hdr[4] == TOC_VER && hdr[5] == enc && cnt <= MP_TOC_MAX &&
            flen == book_len && fcrc == book_crc && st.st_size == expect) {
            *out_count = cnt;
            hit = 1;
        }
    }
    close(fd);
    return hit;
}

static int flush_wr_batch(int tfd, uint16_t *batch_n)
{
    if (*batch_n == 0) {
        return 0;
    }
    size_t bytes = (size_t)(*batch_n) * TOC_ENTRY_BYTES;
    if (write_all_fd(tfd, s_wr_buf, bytes) != 0) {
        return -1;
    }
    *batch_n = 0;
    return 0;
}

static int push_entry(int tfd, uint16_t *batch_n, uint16_t *total_n,
                      uint32_t offset, const char title[MP_TOC_TITLE_BYTES])
{
    if (*total_n >= MP_TOC_MAX) {
        return 0;
    }
    uint8_t *slot = &s_wr_buf[(size_t)(*batch_n) * TOC_ENTRY_BYTES];
    memset(slot, 0, TOC_ENTRY_BYTES);
    wr_le32(slot, offset);
    memcpy(slot + 4u, title, MP_TOC_TITLE_BYTES);
    slot[TOC_ENTRY_BYTES - 1u] = '\0';
    (*batch_n)++;
    (*total_n)++;
    if (*batch_n >= TOC_WR_BATCH) {
        return flush_wr_batch(tfd, batch_n);
    }
    return 0;
}

void mp_toc_delete(uint16_t slot)
{
    if (slot >= MP_MAX_SLOTS) {
        return;
    }
    char path[64];
    toc_path(slot, path, sizeof(path));
    (void)unlink(path);
    toc_tmp_path(slot, path, sizeof(path));
    (void)unlink(path);
}

int mp_toc_ensure(uint16_t slot, uint32_t book_len, uint8_t enc,
                  uint16_t *out_count)
{
    if (slot >= MP_MAX_SLOTS || !out_count) {
        return -1;
    }
    *out_count = 0;
    enc = (enc == MP_ENC_GB2312) ? MP_ENC_GB2312 : MP_ENC_ASCII;

    char bpath[64];
    char tpath[64];
    char tmppath[64];
    book_path(slot, bpath, sizeof(bpath));
    toc_path(slot, tpath, sizeof(tpath));
    toc_tmp_path(slot, tmppath, sizeof(tmppath));

    int bfd = open(bpath, O_RDONLY);
    if (bfd < 0) {
        ESP_LOGW(TAG, "toc book open failed slot=%u", (unsigned)slot);
        return -1;
    }

    uint32_t book_crc = 0;
    if (calc_book_head_crc(bfd, book_len, &book_crc) != 0) {
        close(bfd);
        return -1;
    }

    uint16_t cached = 0;
    if (check_cache_valid(tpath, enc, book_len, book_crc, &cached)) {
        close(bfd);
        *out_count = cached;
        ESP_LOGI(TAG, "toc cache hit slot=%u count=%u",
                 (unsigned)slot, (unsigned)cached);
        return 0;
    }

    /* 未命中缓存：流式顺序扫描全书并原子构建 toc<slot>.bin。 */
    if (lseek(bfd, 0, SEEK_SET) < 0) {
        close(bfd);
        return -1;
    }
    (void)unlink(tmppath);
    int tfd = open(tmppath, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (tfd < 0) {
        close(bfd);
        ESP_LOGW(TAG, "toc tmp open failed slot=%u", (unsigned)slot);
        return -1;
    }

    uint8_t hdr[TOC_HDR_BYTES];
    encode_hdr(hdr, enc, 0, book_len, book_crc);
    if (write_all_fd(tfd, hdr, sizeof(hdr)) != 0) {
        close(tfd);
        close(bfd);
        (void)unlink(tmppath);
        return -1;
    }

    uint8_t line_buf[TOC_LINE_BUF_BYTES];
    size_t line_len = 0;
    uint8_t line_too_long = 0;
    uint32_t line_start_off = 0;
    uint32_t pos = 0;
    uint16_t batch_n = 0;
    uint16_t total_n = 0;
    int io_err = 0;

    while (pos < book_len && total_n < MP_TOC_MAX) {
        uint32_t rem = book_len - pos;
        size_t chunk = rem > sizeof(s_scan_buf) ? sizeof(s_scan_buf)
                                                : (size_t)rem;
        ssize_t got = read(bfd, s_scan_buf, chunk);
        if (got <= 0) {
            io_err = 1;
            break;
        }
        for (ssize_t k = 0; k < got; k++) {
            uint8_t c = s_scan_buf[k];
            uint32_t byte_off = pos + (uint32_t)k;
            if (c == '\n') {
                if (!line_too_long && line_len > 0) {
                    char title[MP_TOC_TITLE_BYTES];
                    uint32_t lead_skip = 0;
                    if (mp_toc_match_line(line_buf, line_len, enc,
                                          title, &lead_skip)) {
                        uint32_t target = line_start_off + lead_skip;
                        if (target < book_len &&
                            push_entry(tfd, &batch_n, &total_n,
                                       target, title) != 0) {
                            io_err = 1;
                            break;
                        }
                    }
                }
                line_len = 0;
                line_too_long = 0;
                line_start_off = byte_off + 1u;
                if (total_n >= MP_TOC_MAX) {
                    break;
                }
            } else if (!line_too_long) {
                if (line_len < sizeof(line_buf)) {
                    line_buf[line_len++] = c;
                } else {
                    line_too_long = 1;
                }
            }
        }
        if (io_err) {
            break;
        }
        pos += (uint32_t)got;
    }

    /* 处理书末不含 '\n' 的最后一行。 */
    if (!io_err && total_n < MP_TOC_MAX && !line_too_long && line_len > 0) {
        char title[MP_TOC_TITLE_BYTES];
        uint32_t lead_skip = 0;
        if (mp_toc_match_line(line_buf, line_len, enc, title, &lead_skip)) {
            uint32_t target = line_start_off + lead_skip;
            if (target < book_len &&
                push_entry(tfd, &batch_n, &total_n, target, title) != 0) {
                io_err = 1;
            }
        }
    }

    if (!io_err && flush_wr_batch(tfd, &batch_n) != 0) {
        io_err = 1;
    }

    encode_hdr(hdr, enc, total_n, book_len, book_crc);
    if (!io_err && (lseek(tfd, 0, SEEK_SET) < 0 ||
                    write_all_fd(tfd, hdr, sizeof(hdr)) != 0 ||
                    fsync(tfd) != 0)) {
        io_err = 1;
    }
    if (close(tfd) != 0) {
        io_err = 1;
    }
    close(bfd);

    if (io_err) {
        (void)unlink(tmppath);
        ESP_LOGW(TAG, "toc scan failed slot=%u", (unsigned)slot);
        return -1;
    }

    (void)unlink(tpath);
    if (rename(tmppath, tpath) != 0) {
        (void)unlink(tmppath);
        ESP_LOGW(TAG, "toc rename failed slot=%u", (unsigned)slot);
        return -1;
    }

    *out_count = total_n;
    ESP_LOGI(TAG, "toc built slot=%u count=%u bytes=%u",
             (unsigned)slot, (unsigned)total_n, (unsigned)book_len);
    return 0;
}

int mp_toc_read_window(uint16_t slot, uint16_t start, uint8_t max_n,
                       mp_toc_entry_t *out)
{
    if (slot >= MP_MAX_SLOTS || !out || max_n == 0) {
        return -1;
    }
    char path[64];
    toc_path(slot, path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    uint8_t hdr[TOC_HDR_BYTES];
    int got = -1;
    if (read_all_fd(fd, hdr, sizeof(hdr)) == 0 &&
        hdr[0] == 'M' && hdr[1] == 'T' && hdr[2] == 'O' && hdr[3] == 'C' &&
        hdr[4] == TOC_VER) {
        uint16_t cnt = rd_le16(&hdr[6]);
        if (cnt > MP_TOC_MAX) {
            cnt = MP_TOC_MAX;
        }
        if (start >= cnt) {
            got = 0;
        } else {
            uint16_t rem = (uint16_t)(cnt - start);
            uint8_t take = rem < max_n ? (uint8_t)rem : max_n;
            off_t off = (off_t)TOC_HDR_BYTES +
                        (off_t)start * (off_t)TOC_ENTRY_BYTES;
            if (lseek(fd, off, SEEK_SET) >= 0) {
                uint8_t rec[TOC_ENTRY_BYTES];
                got = 0;
                for (uint8_t i = 0; i < take; i++) {
                    if (read_all_fd(fd, rec, sizeof(rec)) != 0) {
                        got = -1;
                        break;
                    }
                    out[i].offset = rd_le32(rec);
                    memcpy(out[i].title, rec + 4u, MP_TOC_TITLE_BYTES);
                    out[i].title[MP_TOC_TITLE_BYTES - 1u] = '\0';
                    got++;
                }
            }
        }
    }

    close(fd);
    return got;
}

int mp_toc_read_entry(uint16_t slot, uint16_t index, mp_toc_entry_t *out)
{
    if (!out) {
        return -1;
    }
    return mp_toc_read_window(slot, index, 1u, out) == 1 ? 0 : -1;
}

static int read_offset_at(int fd, uint16_t idx, uint32_t *out_off)
{
    off_t pos = (off_t)TOC_HDR_BYTES + (off_t)idx * (off_t)TOC_ENTRY_BYTES;
    uint8_t b[4];
    if (lseek(fd, pos, SEEK_SET) < 0 || read_all_fd(fd, b, sizeof(b)) != 0) {
        return -1;
    }
    *out_off = rd_le32(b);
    return 0;
}

uint16_t mp_toc_find_for_pos(uint16_t slot, uint16_t count, uint32_t char_pos)
{
    if (slot >= MP_MAX_SLOTS || count == 0) {
        return 0;
    }
    char path[64];
    toc_path(slot, path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }

    uint32_t first_off = 0;
    uint16_t best = 0;
    if (read_offset_at(fd, 0, &first_off) == 0 && first_off <= char_pos) {
        uint16_t lo = 0;
        uint16_t hi = (uint16_t)(count - 1u);
        while (lo <= hi) {
            uint16_t mid = (uint16_t)(lo + ((hi - lo) >> 1));
            uint32_t off = 0;
            if (read_offset_at(fd, mid, &off) != 0) {
                break;
            }
            if (off <= char_pos) {
                best = mid;
                if (mid == hi) {
                    break;
                }
                lo = (uint16_t)(mid + 1u);
            } else {
                if (mid == 0) {
                    break;
                }
                hi = (uint16_t)(mid - 1u);
            }
        }
    }

    close(fd);
    return best;
}

#endif /* MP_TOC_HOST_ONLY */
