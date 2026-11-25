#pragma once
/* storage.h — frozen storage API (DESIGN.md §6, §8, §17.2)
 *
 * SD/FAT only. No function here may touch internal flash, NVS, otadata or
 * any partition (DESIGN §13). Paths are limited to /sdcard/books and the
 * /sdcard/reader subtree (state, tmp).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    ST_OK = 0,
    ST_ERR_NO_SD = -1,
    ST_ERR_IO = -2,
    ST_ERR_NO_SPACE = -3,
    ST_ERR_NOT_FOUND = -4,
    ST_ERR_BAD_PARAM = -5,
    ST_ERR_CRC = -6,
    ST_ERR_SIZE = -7,
    ST_ERR_NAME = -8,
    ST_ERR_BUSY = -9,       /* 取锁超时(通常上传 END 正持锁):稍后重试即可 */
} st_err_t;

typedef struct {
    char name[64];
    char path[192];
    uint32_t id;             /* FNV-1a32(absolute path), printed as 8 hex digits */
    uint64_t size;
} storage_book_t;

typedef struct {
    uint64_t offset;
    uint32_t page;
    uint8_t percent;
    uint8_t enc;
    bool valid;
} storage_pos_t;

int storage_init(void);
bool storage_sd_ok(void);

/* storage_close_book -> unmount -> GPIO34 low 50 ms -> 3 mounts / 500 ms
 * -> rescan. Returns ST_OK or ST_ERR_NO_SD (DESIGN §6.6). */
int storage_remount(void);
/* 关机前安全关闭打开文件并卸载 SD 卡、拉高 GPIO34 断电。 */
void storage_unmount(void);
int storage_rescan(void);
int storage_list(storage_book_t *out, int max);
const char *storage_last_error(void);

int storage_open_book(const storage_book_t *b);

/* 关闭当前书句柄。只等 ST_LOCK_MS(2 s)取 FS 锁:上传 END 会在持锁状态
 * 下把整本书(.part 数据区)拷进 books/ 并 fsync(≤32 MB,可达数十秒),
 * 无限等待会把调用方(UI 任务)冻到 END 结束。
 * 返回:ST_OK 已关闭;ST_ERR_BUSY 锁被 END 占着,句柄保持打开,稍后重试;
 *       ST_ERR_NO_SD 还没有互斥量(未初始化)或卡已失效。 */
st_err_t storage_close_book(void);

/* text_source_t 的 read_at。返回 0 成功;文件尾短读也返回 0(text_source_t
 * 没有长度出参),调用方必须用 src.size 截断请求;off > size 返回
 * ST_ERR_BAD_PARAM;取锁超时(END 正在跑)返回 ST_ERR_BUSY,稍后重试。 */
int storage_read_at(void *ctx, uint64_t off, void *buf, size_t len);
const storage_book_t *storage_current_book(void);

/* 删除一本书:书文件 + 对应边车(按 §6.1 命名:
 * /sdcard/reader/state/<id>.pos、<id>.bmk0、<id>.bmk1)。
 * path 必须是 /sdcard/books/ 下的绝对路径(storage_book_t.path 原样传入,
 * 边车 id 由该路径的 FNV-1a32 算出,换一种写法就算不出同一个 id)。
 * 返回:ST_OK;ST_ERR_BAD_PARAM 路径不在 books/ 下或有 "..";
 *       ST_ERR_NOT_FOUND 书文件本来就不存在(边车仍尽力清理);
 *       ST_ERR_IO 删除/边车清理/重扫失败;ST_ERR_BUSY 取锁超时,可重试。 */
st_err_t storage_delete_book(const char *path);

/* Progress: two 32 B slots with CRC, higher seq wins (DESIGN §6.2).
 * pos_save is best-effort: write, fflush, fsync, read back and CRC-check.
 * pos_set_enc must keep offset/page/percent and rewrite the same slot
 * (DESIGN §6.2 / revision #23).
 *
 * 调用顺序:打开书先 pos_load;有有效进度 → pos_set_enc(只改 enc 位);
 * 没有 → pos_save(id, 起始 offset, page, percent, enc) 一次写全(enc 已在
 * 这条记录里,不需要再调 pos_set_enc)。pos_set_enc 在"没有有效 .pos"时
 * 返回 ST_ERR_NOT_FOUND 且**不建文件** —— 凭空写一个 offset 0 的槽会把
 * 调用方手上的位置冲掉。所有 pos_* 在取锁超时(上传 END 正在持锁)时返回
 * ST_ERR_BUSY,稍后重试即可。 */
int storage_pos_load(uint32_t id, storage_pos_t *out);
int storage_pos_save(uint32_t id, uint64_t offset, uint32_t page, uint8_t percent, uint8_t enc);
int storage_pos_set_enc(uint32_t id, uint8_t enc);
int storage_pos_flush(uint32_t timeout_ms);

/* Bookmarks: .bmk0/.bmk1, generation + per-record CRC, no rename (§6.3). */
int storage_bmk_list(uint32_t id, uint64_t *offs, uint32_t *pages, int max);
int storage_bmk_add(uint32_t id, uint64_t off, uint32_t page);
int storage_bmk_remove(uint32_t id, uint64_t off);

/* Upload: .part file with a 64 B header (DESIGN §6.4/§6.5).
 * flags bit0 = restart (revision #32); resume_off is the existing data size.
 * storage_up_write takes absolute offsets; the caller resends from
 * storage_up_written() when a gap is detected.
 *
 * END 语义:校验 CRC → 把 .part 数据区(跳过 64 B 头)拷进
 * books/<name>.part → 删旧同名书 → rename 发布 → 删 tmp 的 .part。
 * 不能直接 rename .part(64 B 头会变成书的首 64 字节),所以 END 必然多
 * 一次全量写;这一步同时也是发布前的 CRC 复验,掉电恢复路径不变。
 * END 全程持 FS 锁(最长数十秒),期间其它 storage API 等 2 s 后返回
 * ST_ERR_BUSY —— 都是低频路径,调用方(含 UI)重试即可,BLE 侧已按
 * "可重试"处理。 */
int storage_up_begin(const char *name, uint64_t total, uint32_t crc32, uint8_t enc,
                     uint8_t flags, uint64_t *resume_off);
int storage_up_write(uint64_t off, const uint8_t *data, size_t len);
uint64_t storage_up_written(void);
int storage_up_end(uint32_t crc32, char *final_path, size_t n);
int storage_up_abort(void);
