#pragma once
/* E2S 按键：原厂 KeyInit 把 GPIO 2/4/6/7/8 配成输入 + 内部上拉，低电平按下。 */
#include <stdbool.h>
#include <stdint.h>

void key_init(void);
/* 消费一次“按下沿”事件（消抖后）。 */
bool key_take_event(void);
/* 当前按住的键位掩码（GPIO 位图），调试用。 */
uint32_t key_mask(void);
/* 每脚按下次数 + 最后一次按住时长，用来确定 GPIO→物理键映射。 */
void key_stats(char *out, int n);
