#pragma once
/* 板级只读探测：GPIO34 / GPIO5 原始电平。
 *
 * 原厂 1.3.739 反汇编里：
 *   GPIO34 在 SD 挂载前拉低 50 ms，卸载时置高（SD 电源/复位，保持电平未知）；
 *   GPIO5  配成输入，level==0 当布尔返回（疑似充电检测，语义未确认）。
 * 本机是否有这两条电路、电平到底代表什么，仓库里没有任何真机记录。
 * 这里只把脚配成无上下拉输入并读电平，不驱动、不改变外部状态。
 */
void board_probe_init(void);
void board_probe_diag(char *out, int n);
