#pragma once
/* Bad Apple 帧回放：从 flash 原始区 0x530000 的 frames.bin 解码 1bpp 帧并刷新墨水屏。 */
void playback_start(void);
/* 下一帧强制走一次全刷（清残影/重同步 RAM2）。 */
void playback_request_full(void);
/* 触发一次真机时序测量（全刷+局刷，实测约 2 s）；结果从 (1,244) 读。 */
void playback_request_timing(void);
/* 帧区状态，供 A5 diag 查询：fr=ok,c=1314,fps=6 / fr=no_data ... */
void playback_diag(char *out, int n);
