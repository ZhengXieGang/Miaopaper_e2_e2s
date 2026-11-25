/* text_encode.c — 编码识别与开书(DESIGN §5.2、§8)。
 *
 * 整个文件做一次流式扫描:32 KB 缓冲是 **static**(DESIGN §3.2/§14 #36,
 * 不能放栈)。同时累加两套计数:
 *   - utf8_bad / utf8_seq:严格 UTF-8(拒绝过长、代理区、越界、>U+10FFFF),
 *     跨块未完成序列暂存到下一块,EOF 仍未完成记 1 个 bad;
 *   - gbk_bad / gbk_seq:GBK 双字节规则(首 0x81..0xFE,次 0x40..0xFE 且 ≠0x7F),
 *     ASCII 单字节通过,非法对记 1 个 bad。
 * 判定:utf8_bad==0 → UTF-8;否则 gbk_bad==0 → GBK;都非 0 → bad/序列数较小者,
 * 相等取 GBK;纯 ASCII(两者都为 0)落在第一条 → UTF-8。
 * forced(来自 .pos 的持久化 enc)优先,不再扫描(DESIGN §5.2 第 5 条)。
 *
 * 线程模型:文本层只由 app 任务调用(DESIGN §3.1),因此 static 扫描缓冲安全。
 */
#include <stddef.h>
#include <stdint.h>

#include "text.h"

#include <string.h>

#include "text_internal.h"

static const char *TAG = "text";

/* DESIGN §3.2/§14 #36:32 KB 扫描缓冲必须是 static,**不能放栈**。 */
#define TEXT_SCAN_BUF_BYTES (32u * 1024u)
static uint8_t s_scan_buf[TEXT_SCAN_BUF_BYTES];

/* ------------------------------------------------------------------ *
 * UTF-8 扫描状态机
 * ------------------------------------------------------------------ */
typedef struct {
    uint8_t  pend[4];   /* 跨块未完成的序列 */
    uint8_t  pend_len;
    uint64_t seqs;      /* 单位数(ASCII 字节/多字节字符) */
    uint64_t bad;       /* 非法序列数 */
} tx_utf8_scan_t;

static void u8_feed(tx_utf8_scan_t *st, const uint8_t *p, size_t n)
{
    size_t i = 0;

    /* 先补齐上一块遗留的序列 */
    while (st->pend_len > 0 && i < n) {
        int len = tx_utf8_seq_len(st->pend[0]);
        if (len <= 0) {
            /* 不可能发生:入暂存前已确认首字节合法 */
            st->pend_len = 0;
            break;
        }
        st->pend[st->pend_len++] = p[i++];
        if ((int)st->pend_len == len) {
            uint32_t cp;
            st->seqs++;
            if (!tx_utf8_check(st->pend, len, &cp)) {
                st->bad++;
            }
            st->pend_len = 0;
        }
    }

    while (i < n) {
        uint8_t b = p[i];
        if (b < 0x80u) {
            st->seqs++;
            i++;
            continue;
        }
        int len = tx_utf8_seq_len(b);
        if (len == 0) {
            st->seqs++;
            st->bad++;
            i++;
            continue;
        }
        if (i + (size_t)len > n) {
            st->pend_len = (uint8_t)(n - i);
            memcpy(st->pend, p + i, st->pend_len);
            return;
        }
        uint32_t cp;
        st->seqs++;
        if (!tx_utf8_check(p + i, len, &cp)) {
            st->bad++;
        }
        i += (size_t)len;
    }
}

static void u8_finish(tx_utf8_scan_t *st)
{
    if (st->pend_len > 0) {
        /* EOF 仍未完成:记 1 个 bad(DESIGN §5.2) */
        st->seqs++;
        st->bad++;
        st->pend_len = 0;
    }
}

/* ------------------------------------------------------------------ *
 * GBK 扫描状态机
 * ------------------------------------------------------------------ */
typedef struct {
    uint8_t  pend;      /* 0 或待配对的 GBK 首字节 */
    uint64_t seqs;
    uint64_t bad;
} tx_gbk_scan_t;

static void gb_feed(tx_gbk_scan_t *st, const uint8_t *p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t b = p[i];
        if (st->pend) {
            st->pend = 0;
            if (tx_gbk_trail_ok(b)) {
                st->seqs++;
                i++;
            } else {
                /* 非法对记 1 个 bad,不消费当前字节(它可能是新的首字节) */
                st->seqs++;
                st->bad++;
            }
            continue;
        }
        if (b < 0x80u) {
            st->seqs++;
            i++;
        } else if (tx_gbk_lead_ok(b)) {
            st->pend = b;
            i++;
        } else {
            st->seqs++;
            st->bad++;
            i++;
        }
    }
}

static void gb_finish(tx_gbk_scan_t *st)
{
    if (st->pend) {
        st->seqs++;
        st->bad++;
        st->pend = 0;
    }
}

/* ------------------------------------------------------------------ *
 * 判定(DESIGN §5.2 第 3/4 条)
 * ------------------------------------------------------------------ */
static text_enc_t tx_decide(uint64_t u_bad, uint64_t u_seq,
                            uint64_t g_bad, uint64_t g_seq)
{
    if (u_bad == 0) {
        return TEXT_ENC_UTF8;   /* 含纯 ASCII:两个计数都为 0 */
    }
    if (g_bad == 0) {
        return TEXT_ENC_GBK;
    }
    /* 比较 bad/seq。序列数上限 32 MB,乘积 < 2^50,不会溢出 64 位。 */
    uint64_t lhs = u_bad * (g_seq ? g_seq : 1u);
    uint64_t rhs = g_bad * (u_seq ? u_seq : 1u);
    if (lhs < rhs) {
        return TEXT_ENC_UTF8;
    }
    return TEXT_ENC_GBK;        /* 相等取 GBK */
}

/* 全文件流式双计数。返回 TEXT_OK 或负错误码。 */
static int tx_scan_encoding(text_book_t *b, text_enc_t *out)
{
    tx_utf8_scan_t us = {0};
    tx_gbk_scan_t gs = {0};
    uint64_t off = 0;

    while (off < b->size) {
        size_t want = TEXT_SCAN_BUF_BYTES;
        if (b->size - off < (uint64_t)want) {
            want = (size_t)(b->size - off);
        }
        if (b->src.read_at(b->src.ctx, off, s_scan_buf, want) != 0) {
            TX_LOGE(TAG, "encoding scan read_at(%llu, %u) failed",
                    (unsigned long long)off, (unsigned)want);
            return TEXT_ERR_IO;
        }
        u8_feed(&us, s_scan_buf, want);
        gb_feed(&gs, s_scan_buf, want);
        off += want;
    }
    u8_finish(&us);
    gb_finish(&gs);

    text_enc_t enc = tx_decide(us.bad, us.seqs, gs.bad, gs.seqs);
    TX_LOGI(TAG, "scan %llu B: utf8 %llu bad/%llu seq, gbk %llu bad/%llu seq -> %s",
            (unsigned long long)b->size,
            (unsigned long long)us.bad, (unsigned long long)us.seqs,
            (unsigned long long)gs.bad, (unsigned long long)gs.seqs,
            text_enc_name(enc));
    *out = enc;
    return TEXT_OK;
}

/* ------------------------------------------------------------------ *
 * text_open
 * ------------------------------------------------------------------ */
int text_open(text_book_t *b, const text_source_t *src, text_enc_t forced)
{
    if (!b || !src || !src->read_at) {
        return TEXT_ERR_PARAM;
    }
    memset(b, 0, sizeof(*b));
    b->src = *src;
    b->size = src->size;

    if (b->size > READER_UPLOAD_MAX_BYTES) {
        TX_LOGE(TAG, "book too large: %llu B > %llu B",
                (unsigned long long)b->size,
                (unsigned long long)READER_UPLOAD_MAX_BYTES);
        return TEXT_ERR_TOO_BIG;
    }

    /* 持久化的编码优先,不再扫描(DESIGN §5.2 第 5 条;0x26 覆盖后重新持久化) */
    if (forced == TEXT_ENC_UTF8 || forced == TEXT_ENC_GBK) {
        b->enc = forced;
        TX_LOGI(TAG, "encoding forced to %s (%llu B)",
                text_enc_name(forced), (unsigned long long)b->size);
        return TEXT_OK;
    }

    /* 前 3 B 判 BOM(DESIGN §5.2 第 1 条) */
    uint8_t bom[3] = {0};
    size_t have = (b->size < 3u) ? (size_t)b->size : 3u;
    if (have > 0) {
        if (b->src.read_at(b->src.ctx, 0, bom, have) != 0) {
            TX_LOGE(TAG, "BOM read failed");
            return TEXT_ERR_IO;
        }
    }
    if (have >= 3 && bom[0] == 0xEFu && bom[1] == 0xBBu && bom[2] == 0xBFu) {
        b->enc = TEXT_ENC_UTF8;
        TX_LOGI(TAG, "UTF-8 BOM -> UTF-8");
        return TEXT_OK;
    }
    if (have >= 2 && bom[0] == 0xFFu && bom[1] == 0xFEu) {
        TX_LOGE(TAG, "UTF-16LE BOM unsupported");
        return TEXT_ERR_UNSUP;
    }
    if (have >= 2 && bom[0] == 0xFEu && bom[1] == 0xFFu) {
        TX_LOGE(TAG, "UTF-16BE BOM unsupported");
        return TEXT_ERR_UNSUP;
    }

    if (b->size == 0) {
        b->enc = TEXT_ENC_UTF8;      /* 空文件:按纯 ASCII 处理 */
        return TEXT_OK;
    }
    return tx_scan_encoding(b, &b->enc);
}

/* ------------------------------------------------------------------ *
 * 小工具
 * ------------------------------------------------------------------ */
int text_percent(const text_book_t *b, uint64_t pos)
{
    if (!b || b->size == 0) {
        return 0;
    }
    if (pos >= b->size) {
        return 100;
    }
    return (int)((pos * 100u) / b->size);
}

const char *text_enc_name(text_enc_t enc)
{
    switch (enc) {
    case TEXT_ENC_UTF8:
        return "UTF-8";
    case TEXT_ENC_GBK:
        return "GBK";
    case TEXT_ENC_AUTO:
    default:
        return "auto";
    }
}
