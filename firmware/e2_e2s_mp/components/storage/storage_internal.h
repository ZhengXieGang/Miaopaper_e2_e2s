#pragma once
/* storage_internal.h — storage 包私有定义(不是公共 API)。
 *
 * DESIGN.md §6.2/§6.3/§6.4 冻结了这里的偏移与魔数;四个 .c 文件共享这些
 * 常量。下面的 st_* 辅助函数只在 storage 组件内部链接,不属于 storage.h
 * 冻结的接口,其他组件不得依赖。
 */
#include <stdbool.h>
#include <stdint.h>

#include "reader_cfg.h"
#include "storage.h"

#define ST_TAG "storage"

/* 路径缓冲(READER_DIR_STATE + "/" + 8 位 id + 后缀) */
#define ST_PATH_MAX 192
#define ST_ID_HEX_LEN 8

/* 所有阻塞式取锁的超时;0xFFFFFFFF 表示永久等待(只有 storage_remount
 * 可以用:它必须先拿到锁才能卸载挂载点)。关书同样走 ST_LOCK_MS ——
 * 上传 END 持锁拷贝整本书时返回 ST_ERR_BUSY,由调用方重试。 */
#define ST_LOCK_MS 2000u
#define ST_WAIT_FOREVER 0xFFFFFFFFu

/* ---- 进度(64 B = 2×32 B 槽,§6.2)---- */
#define ST_POS_SLOT_BYTES 32
#define ST_POS_FILE_BYTES (2 * ST_POS_SLOT_BYTES)
#define ST_POS_MAGIC 'P'
#define ST_POS_VERSION 1
#define ST_POS_FLAG_ENC_SHIFT 4
#define ST_POS_FLAG_ENC_MASK (0x3u << ST_POS_FLAG_ENC_SHIFT)
#define ST_POS_OFF_OFFSET 8
#define ST_POS_OFF_PAGE 16
#define ST_POS_OFF_PERCENT 20
#define ST_POS_OFF_CRC 28

/* ---- 书签(16 B 头 + 24 B 记录 + 8 B 尾,§6.3)---- */
#define ST_BMK_HEADER_BYTES 16
#define ST_BMK_RECORD_BYTES 24
#define ST_BMK_TAIL_BYTES 8
#define ST_BMK_MAGIC 0x424D4B45u   /* "BMKE" */
#define ST_BMK_FILE_MAGIC "BMK1"
#define ST_BMK_ACTIVE_MAX 8
#define ST_BMK_MAX_FILE_BYTES 8192

/* ---- 上传暂存(64 B 头,§6.4)---- */
#define ST_UP_HEADER_BYTES 64
#define ST_UP_MAGIC "UPL1"
#define ST_UP_FLAG_BAD 0x01
#define ST_UP_FLAG_RESTART 0x01
/* 头部 name 字段:≤39 B + NUL(§6.1,READER_UPLOAD_NAME_MAX = 39) */
#define ST_UP_NAME_BYTES 40
/* 每写满这么多字节做一次 f_sync,掉电最多丢 8 KB 上传数据 */
#define ST_UP_SYNC_BYTES 8192

/* ---- 互斥与 SD 状态(storage_sd.c 实现)---- */

/* 只取递归互斥量,不检查卡状态(关句柄/重挂用)。 */
int st_lock(uint32_t timeout_ms);
void st_unlock(void);

/* 取锁 + 检查 s_sd_ok/已挂载;失败返回 ST_ERR_NO_SD 或 ST_ERR_BUSY,
 * 失败时没有持锁,调用方直接返回即可。 */
int st_fs_take(uint32_t timeout_ms);

bool st_sd_is_ok(void);
void st_sd_fail(const char *why);        /* 置 s_sd_ok=false + 记录原因 */
void st_sd_set_error(const char *msg);   /* 只记录 storage_last_error */

/* ---- 书目缓存(storage_lib.c 实现,调用方须持有 FS 锁)---- */
int st_lib_scan(void);

/* ---- 上传(storage_upload.c 实现,调用方须持有 FS 锁)----
 * 重挂/卸载前关掉 .part 的 FILE*,避免卸载后留下悬空句柄;.part 文件保留,
 * 下次 BEGIN 用文件大小续传。 */
void st_up_force_close(void);
