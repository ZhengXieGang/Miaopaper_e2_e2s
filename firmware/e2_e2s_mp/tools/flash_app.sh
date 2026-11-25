#!/usr/bin/env bash
# =============================================================================
# tools/flash_app.sh — 只打印 e2s_mp 的 app-only 烧写命令,不执行
#
# 本脚本不会调用 esptool、不会打开串口、不会读写 flash。它只把命令打印到
# stdout,由使用者自行复制执行(这是刻意的:避免误刷)。tools/build.sh 同样
# 拒绝 flash/app-flash/otadata-flash 等一切写设备的子命令。
#
# ！！先读警告！！
#
#   1. e2s_mp 是 app-only 镜像。真机上只允许写:
#          ota_0 = 0x20000   (大小 0x1F0000)
#          ota_1 = 0x210000  (大小 0x1F0000)
#      两个地址来自 8 MiB dump 的 0x8000 分区表(PORT_SPEC §8.1)。
#
#   2. 绝对不要写:
#          0x000000  bootloader
#          0x008000  分区表
#          0x009000  NVS(真机 0x9000..0xF000)
#          0x00F000  真机 otadata(0xF000..0x11000)
#          0x011000  phy_init
#          0x010000  真机 otadata 内部(0xF000..0x11000),不是 app
#          0x200000  真机 ota_0 尾部,不是 ota_1 起点
#          0x480000  原厂字库区(0x480000..0x528884)
#
#   3. 工具/人工不要为了切换槽位改写 otadata。唯一的例外是固件内的“恢复
#      原厂”(PORT_SPEC §8.1):它先校验原厂槽,再由用户确认后写一条记录。
#
#   4. partitions.csv 现在就是真机表(从 dump 的 0x8000 解出,gen_esp32part.py
#      核对),所以 build/flash_args 写的是 0xf000 / 0x20000。这不代表
#      `idf.py flash` 变安全:它仍会写 bootloader(0x0)、分区表(0x8000)和
#      ota_data_initial(0xf000,真机 otadata)。永远不要运行 `idf.py flash`,
#      只使用本脚本打印的 app-only 命令。
#
# 用法:
#   ./tools/flash_app.sh [--port /dev/ttyACM0] [--slot ota_0|ota_1]
#                        [--otadata /tmp/otadata.bin]
#                        [--image build/miaopaper_e2_e2s.bin] [--baud 460800]
#
# 说明:
#   - 推荐 --otadata:传入前面 read-flash 0xf000 0x2000 得到的本地 dump,
#     脚本按 ESP-IDF bootloader 规则解析出**活动槽**,只打印那一条写命令。
#   - --slot 只给已经人工确认活动槽的人用;不带 --slot 也不带 --otadata 时
#     只打印 otadata 只读回读命令,不打印任何写命令(避免两条并列被误抄)。
#   - 本脚本不读 flash、不打开串口,只读本地 dump 并打印命令。
#   - 镜像超过 0x1F0000 时退出码 3,不会打印写命令。
#
# 退出码:0 = 已打印;2 = 参数错误;3 = 镜像缺失/超尺寸。
# =============================================================================
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

PORT="/dev/ttyACM0"
SLOT=""
OTADATA=""
IMAGE=""
BAUD="460800"

usage() {
    cat <<'EOF'
tools/flash_app.sh — 只打印 e2s_mp 的 app-only 烧写命令,不执行

用法:
  ./tools/flash_app.sh --otadata /tmp/otadata.bin [选项]
  ./tools/flash_app.sh --slot ota_0|ota_1 [选项]

  --port     串口设备(默认 /dev/ttyACM0)
  --slot     ota_0(0x20000) 或 ota_1(0x210000);仅限已人工确认活动槽
  --otadata  本地 read-flash 0xf000 0x2000 得到的 dump;自动选活动槽
  --image    app 镜像(默认 build/miaopaper_e2_e2s.bin)
  --baud     esptool 波特率(默认 460800)
  -h/--help  显示本帮助

--slot 与 --otadata 二选一;都不给时只打印 otadata 只读回读命令。
真机只允许写 0x20000 / 0x210000;不要写 0x0 / 0x8000 / 0x9000 / 0xf000 /
0x11000 / 0x480000。永远不要运行 idf.py flash。脚本本身不打开串口。
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --port)   PORT="${2:?--port needs a value}"; shift 2 ;;
        --slot)   SLOT="${2:?--slot needs a value}"; shift 2 ;;
        --otadata) OTADATA="${2:?--otadata needs a value}"; shift 2 ;;
        --image)  IMAGE="${2:?--image needs a value}"; shift 2 ;;
        --baud)   BAUD="${2:?--baud needs a value}"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "flash_app.sh: unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

IMAGE="${IMAGE:-$ROOT/build/miaopaper_e2_e2s.bin}"

if [ ! -f "$IMAGE" ]; then
    echo "flash_app.sh: image not found: $IMAGE" >&2
    echo "              run: (cd \"$ROOT\" && ./tools/build.sh build)" >&2
    exit 3
fi

SIZE=$(wc -c < "$IMAGE" | tr -d '[:space:]')
CAP_REAL=2031616     # 0x1F0000, 真机槽的上限

if [ "$SIZE" -gt "$CAP_REAL" ]; then
    echo "flash_app.sh: image is $SIZE B, larger than the real OTA slot ($CAP_REAL B)" >&2
    exit 3
fi

if [ -n "$SLOT" ] && [ -n "$OTADATA" ]; then
    echo "flash_app.sh: --slot and --otadata are mutually exclusive" >&2
    exit 2
fi

# 按 ESP-IDF bootloader 的规则解析本地 dump：每个 entry 的 CRC 只覆盖
# ota_seq 的 4 字节（bootloader_common_ota_select_crc），有效项取 seq 最大者，
# 活动槽 = (seq - 1) % 2。两个都无效时 bootloader 默认 ota_0。
if [ -n "$OTADATA" ]; then
    if [ ! -f "$OTADATA" ]; then
        echo "flash_app.sh: otadata dump not found: $OTADATA" >&2
        exit 3
    fi
    SLOT=$(python3 - "$OTADATA" <<'PY'
import struct
import sys
import zlib

data = open(sys.argv[1], "rb").read()
if len(data) < 0x1020:
    sys.stderr.write("otadata dump too short: %d B (need 0x1020)\n" % len(data))
    sys.exit(2)

valid = []
for idx, off in enumerate((0, 0x1000)):
    e = data[off:off + 32]
    seq = struct.unpack_from("<I", e, 0)[0]
    state = struct.unpack_from("<I", e, 24)[0]
    crc = struct.unpack_from("<I", e, 28)[0]
    if seq == 0xFFFFFFFF:
        continue
    if state in (3, 4):                      # INVALID / ABORTED
        continue
    if (zlib.crc32(e[:4]) ^ 0xFFFFFFFF) != crc:
        continue
    valid.append((seq, idx))

if not valid:
    sys.stderr.write("otadata: no valid entry; bootloader default ota_0\n")
    print("ota_0")
else:
    seq, idx = max(valid)
    slot = "ota_0" if (seq - 1) % 2 == 0 else "ota_1"
    sys.stderr.write("otadata: active entry %d seq=%d -> %s\n" % (idx, seq, slot))
    print(slot)
PY
) || { echo "flash_app.sh: failed to parse $OTADATA" >&2; exit 3; }
fi

case "$SLOT" in
    "")        SLOTS="" ;;
    ota_0)     SLOTS="ota_0:0x20000" ;;
    ota_1)     SLOTS="ota_1:0x210000" ;;
    *) echo "flash_app.sh: --slot must be ota_0 or ota_1 (got '$SLOT')" >&2; exit 2 ;;
esac

SIZE_HEX=$(printf '0x%x' "$SIZE")

print_slot() {
    _name=$1
    _off=$2
    _end=$(printf '0x%x' $((_off + SIZE)))
    cat <<EOF
# =============================================================================
# 烧写 $_name ($_off)
# =============================================================================
python -m esptool --chip esp32s3 -p $PORT -b $BAUD \\
  --before default-reset --after hard-reset \\
  write-flash --flash-mode dio --flash-freq 80m \\
  $_off $IMAGE

# 结束偏移: $_end  (必须 <= 槽尾)
# USB-Serial-JTAG 口(/dev/ttyACM0)可把 --before default-reset 换成
# --before usb-reset。普通 USB-UART 桥通常是 /dev/ttyUSB0。

# 只读:写后回读比对
python -m esptool --chip esp32s3 -p $PORT -b $BAUD \\
  read-flash $_off $SIZE /tmp/app_readback_$_name.bin
sha256sum $IMAGE /tmp/app_readback_$_name.bin

EOF
}

cat <<EOF
# =============================================================================
# e2s_mp app-only 烧写命令(由 tools/flash_app.sh 生成)
#
# 本脚本没有执行任何命令。下面是给你复制执行的。
# =============================================================================
#
# 镜像      : $IMAGE
# 大小      : $SIZE B ($SIZE_HEX)
# 串口      : $PORT
#
# 警告:只写 app 槽。不要写 0x0 / 0x8000 / 0x9000 / 0xf000 / 0x11000 /
#       0x480000,也不要手工改写 otadata 来切换槽位(固件内“恢复原厂”除外)。
#
# -----------------------------------------------------------------------------
# 1) 只读:确认 otadata 当前指向哪个槽(不改任何东西)
# -----------------------------------------------------------------------------
. ~/esp/esp-idf-v6.1.1/export.sh

python -m esptool --chip esp32s3 -p $PORT -b $BAUD \\
  read-flash 0xf000 0x2000 /tmp/otadata.bin

# 真机表:otadata 在 0xf000..0x11000。两个 entry 的有效项决定活动槽;
# 你的设备必须自己读回确认,不要照抄别处的结论。

EOF

if [ -z "$SLOTS" ]; then
    cat <<'EOF'
# -----------------------------------------------------------------------------
# 未指定 --slot/--otadata:上面只打印了 otadata 只读回读命令,没有写命令。
# 回读后二选一重跑(不要同时写两个槽):
#   ./tools/flash_app.sh --otadata /tmp/otadata.bin
#   ./tools/flash_app.sh --slot ota_0     # 或 ota_1,需人工确认活动槽
# -----------------------------------------------------------------------------
EOF
    exit 0
fi

for _pair in $SLOTS; do
    print_slot "${_pair%%:*}" "${_pair##*:}"
done

cat <<'EOF'
# =============================================================================
# 如果写失败:重新进 ROM 下载模式再试;不要用 idf.py flash,也不要改 otadata
# "修一下"。
# =============================================================================
EOF

exit 0
