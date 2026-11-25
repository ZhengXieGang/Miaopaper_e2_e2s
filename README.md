# 喵喵机 E2 / E2S 墨水屏固件与刷机助手

面向 **喵喵机（Paperang）E2 / E2S**（ESP32-S3 + 2.13 英寸 250×122 墨水屏）的第三方固件研究资料、阅读固件文档，以及安卓无线刷机助手源码。

本仓库只收录**关键文档与刷机 App 源码**；原厂固件、整片 flash dump、字体/美术素材、签名密钥等一律不入库。

## 目录

| 路径 | 内容 |
|---|---|
| [`docs/`](docs/) | 原厂固件逆向与第三方固件开发文档 |
| [`docs/E2S_READER_INVENTORY.md`](docs/E2S_READER_INVENTORY.md) | 硬件/固件资产盘点：引脚、SPI、分区、面板、按键、ADC、SD |
| [`docs/E2S_THIRD_PARTY_FIRMWARE.md`](docs/E2S_THIRD_PARTY_FIRMWARE.md) | 第三方固件经验总结：启动链、OTA 恢复、BLE、EPD、回屏、构建 |
| [`docs/E2_VS_E2S_FIRMWARE_DIFF.md`](docs/E2_VS_E2S_FIRMWARE_DIFF.md) | E2 1.3.759 与 E2S 1.3.739 app-only 固件差异分析 |
| [`dump/FLASH_DUMP_FINDINGS.md`](dump/FLASH_DUMP_FINDINGS.md) | 8 MiB flash dump 记录：分区表、otadata、NVS 键、字体区（仅记录，dump 不入库） |
| [`demo/E2SFlash/`](demo/E2SFlash/) | 安卓刷机助手（Compose / Material 3），通过原厂 OTA 通道无线刷入 app-only 镜像 |
| [`firmware/e2_e2s_mp/README.md`](firmware/e2_e2s_mp/README.md) | E2/E2S 阅读固件（Miaopaper 兼容）说明 |
| [`firmware/e2_e2s_mp/docs/`](firmware/e2_e2s_mp/docs/) | 阅读固件的构建、设计、按键、BLE 协议与施工规范文档 |

## 刷机助手

`demo/E2SFlash` 是一个不绑定 MAC/SN 的通用刷机 App：

- 扫描广播名 `Paperang_E2` / `Paperang_E2S` 的设备，任选一台；
- 固件由用户通过系统文件选择器选择本地 ESP32-S3 **app-only** `.bin`，APK 不内置任何镜像；
- 刷入时先核对型号、电量与镜像格式，再经原厂固件的 A5/OTA 服务写入非运行槽并切换启动；
- 支持“恢复原厂”：只切回原厂 OTA 槽，不上传镜像。

构建（需要 JDK 17+、Android SDK 36、Gradle 9.x）：

```sh
cd demo/E2SFlash
gradle assembleDebug                 # 产物 app/build/outputs/apk/debug/
gradle assembleRelease               # 需要在根目录提供 keystore.properties 才会签名
```

`keystore.properties`、`local.properties`、`keystore/` 属于本地私密配置，已在 `.gitignore` 中排除，仓库不包含任何签名材料。

## 阅读固件文档

`firmware/e2_e2s_mp/docs/` 包含 `BUILD.md`（构建与手动刷写）、`DESIGN.md`（架构）、`KEYMAP.md`（按键与逐屏操作）、`PROTOCOL.md`（BLE 协议与上传）、`COPY_DECK.md`（屏上文案）、`PORT_SPEC.md`（施工规范）。

> 本仓库**暂不收录**固件源码与字体二进制：`components/text/assets/NOTICE.md` 说明 HZK16 字模许可不明，不得随发行物分发；需要构建时请按该说明用 `tools/gen_hz16.py --from-ttf <可再分发的 CJK TTF>` 重新生成字库。

## 脱敏与不入库内容

入库前已做以下处理：

- 未收录真实设备 **SN / MAC / 绑定信息 / 授权 key / OTA token / aesKey**；
- 未收录 NVS dump、整片 flash dump、原厂 OTA 响应与云端凭证；
- 未收录 App 签名密钥（`.jks`）、`keystore.properties`、`local.properties`；
- 文档中的本机绝对路径改为相对路径或省略，源码中的本机 JDK/SDK 路径已移除；
- 未收录原厂 1bpp 美术资源、Zpix / Alibaba PuHuiTi 字体、APK、反编译产物、3D 模型与视频素材。

文档中出现的 `firmware/e2s_re/`、`tools/`、`build/`、`refs/` 等路径来自原始分析工作区，对应二进制与脚本**不在本仓库**；这些引用仅用于说明结论来源。

## 风险与声明

- **刷写第三方固件有风险**：写错槽位、镜像损坏或固件在 BLE 启动前崩溃，都可能需要 UART/esptool 恢复，极端情况下设备无法启动。请仅对自己拥有的设备操作。
- 本项目为**互操作性研究与个人备份**用途，与原厂无关联；`Paperang`、`喵喵机` 等商标归其权利人所有。
- 原厂固件、字体与美术资源版权归原权利人；仓库不包含这些内容。
- 文档中的寄存器、引脚、时序与协议结论来自静态分析与真机验证记录，未做量产级验证，按“现状”提供，不附带任何担保。

## 相关项目

- [Miaopaper](https://github.com/hrp518/Miaopaper)：BLE 协议、分页逻辑与网页上传工具；
- [ATC_TLSR_Paper](https://github.com/atc1441/ATC_TLSR_Paper)：早期逆向资料与界面参考。
