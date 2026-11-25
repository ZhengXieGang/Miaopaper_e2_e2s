#pragma once
/* display_internal.h — display-package private constants (not a public API).
 * DESIGN.md §3.2 ownership handshake and §4.4 GC scheduling.
 */
#include "display.h"

/* 局刷 settle 常量只有一份定义,在 e2s_epd 的公共头 epd.h(D7):
 * 之前这里与 epd.c 各复制一份 500,改一处会漂移。display 私有代码通过
 * 本包含入即可使用 EPD_PARTIAL_SETTLE_MS(display 的 CMake PRIV_REQUIRES
 * 已包含 e2s_epd)。 */
#include "epd.h"

#define DISPLAY_TASK_NAME "display"
#define DISPLAY_TASK_STACK 4096
#define DISPLAY_TASK_PRIO 5
#define DISPLAY_TASK_CORE 1

/* display task wakes on post or this timeout to evaluate GC
 * (DESIGN §4.4: 1 s semaphore timeout). */
#define DISPLAY_WAKE_TIMEOUT_MS 1000
