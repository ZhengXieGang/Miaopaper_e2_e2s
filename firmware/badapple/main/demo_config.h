#pragma once

/* 演示固件的可修改项：改这里即可做自己的固件。 */

/* 屏幕中央显示的文字（8x16 字体，自动居中、2 倍放大）。 */
#define DEMO_TEXT "Bad Apple!!"

/* A5 (1,7) 返回的版本号。原厂版本形如 1.3.739；自定义固件请用别的格式，
 * 刷机 App 用“是否为 x.y.z”来区分原厂与自定义固件。 */
#define DEMO_VERSION "badapple-e2s"

/* BLE 广播名。刷机 App 按这个名字扫描 E2S；改成别的名字会扫不到。 */
#define DEMO_BLE_NAME "Paperang_E2S"
