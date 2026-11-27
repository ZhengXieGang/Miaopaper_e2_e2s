#pragma once
/* mp_toc.h — 电子书自动章节识别与 SD 缓存索引模块。
 *
 * 索引文件仅存放在 /sdcard/mp/toc<slot>.bin，绝不触碰内部 Flash、
 * NVS 或分区表；RAM 中不常驻完整章节表，目录页按需分页读取可见 4 行。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MP_TOC_MAX 2048u          /* 单本书最多索引的章节数 */
#define MP_TOC_TITLE_BYTES 32u    /* 章节标题缓冲长度（含结尾 NUL） */

typedef struct {
    uint32_t offset;                      /* 章节标题在书文件中的精确字节偏移 */
    char title[MP_TOC_TITLE_BYTES];       /* 去除首尾空白并按编码边界安全截断的标题 */
} mp_toc_entry_t;

/* 判断一行原始字节（不含 '\n'）是否为章节标题。
 * 命中时返回 true，并将清理后的标题写入 out_title，同时在 out_lead_skip 中
 * 返回跳过的前导空白字节数（用于定位章节标题精确起点）。
 * enc 取 MP_ENC_ASCII (0) 或 MP_ENC_GB2312 (1)。 */
bool mp_toc_match_line(const uint8_t *line, size_t len, uint8_t enc,
                       char out_title[MP_TOC_TITLE_BYTES],
                       uint32_t *out_lead_skip);

#ifndef MP_TOC_HOST_ONLY
/* 校验或按需扫描构建指定书槽的章节索引缓存。
 * 命中缓存时直接返回 0；未命中则流式扫描 /sdcard/mp/b<slot>.bin 并原子落盘
 * /sdcard/mp/toc<slot>.bin。out_count 允许为 0（无匹配章节）。 */
int mp_toc_ensure(uint16_t slot, uint32_t book_len, uint8_t enc,
                  uint16_t *out_count);

/* 从缓存文件读取从 start 开始的最多 max_n 条章节记录。
 * 返回实际读出的条数（0..max_n），读失败返回 -1。 */
int mp_toc_read_window(uint16_t slot, uint16_t start, uint8_t max_n,
                       mp_toc_entry_t *out);

/* 读取单条章节记录（index < count），成功返回 0，失败返回 -1。 */
int mp_toc_read_entry(uint16_t slot, uint16_t index, mp_toc_entry_t *out);

/* 二分查找当前阅读字节偏移 char_pos 所属的最近章节下标（满足 offset <= char_pos
 * 的最大下标；若所有章节均在 char_pos 之后或 count==0 则返回 0）。 */
uint16_t mp_toc_find_for_pos(uint16_t slot, uint16_t count, uint32_t char_pos);

/* 删除指定书槽对应的章节索引缓存与临时文件。 */
void mp_toc_delete(uint16_t slot);
#endif
