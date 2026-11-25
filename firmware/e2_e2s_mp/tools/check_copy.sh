#!/bin/sh
# tools/check_copy.sh — 屏显文案 lint（COPY_DECK.md §6 / PORT_SPEC §11.5 C1/C4）。
#
#   sh tools/check_copy.sh        # 0 = 通过；1 = 有命中
#
# 检查 main/*.c 里所有字符串字面量（去掉注释、跳过 #include/_Static_assert）：
#   1) COPY_DECK §1.2 的中文禁止词（含“快刷/全刷”）；
#   2) 英文键名 MENU/OK/FUNC/PREV/NEXT/Settings/Back/Locked/Loading/Error；
#   3) 插卡/SD 卡/内存卡；
#   4) 宽度（8 px/ASCII、16 px/非 ASCII，保守门）：普通行 <=234，状态栏标题
#      <=112，整页消息原因行 <=112、动作行 <=96，底栏动词 <=32（“下一步”
#      <=48）。UI v2 实际用 Inter 比例宽，sim --check 才是像素真源。
# 本脚本只读源码，不改文件；需要 python3。
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
exec python3 - "$ROOT" <<'PY'
import glob
import os
import re
import sys

root = sys.argv[1]
files = sorted(glob.glob(os.path.join(root, "main", "*.c")))
if not files:
    print("copy: no main/*.c found")
    sys.exit(1)

FORBIDDEN = ["请", "是否", "确认要", "操作成功", "操作失败", "正在",
             "温馨提示", "注意", "快刷", "全刷"]
FORBIDDEN_ALLOW = {"请充电"}
KEYNAMES = re.compile(r"\b(MENU|OK|FUNC|PREV|NEXT|Settings|Back|Locked|Loading|Error)\b")
SD_WORDS = ["插卡", "SD卡", "SD 卡", "内存卡"]

# 宽度门（COPY_DECK §1.5）；不在表里的普通字面量按 <=234 行宽。
STATUS_TITLES = {"书库", "设置", "关于", "存储错误", "无法恢复", "刷新失败",
                 "屏幕不可用", "恢复原厂固件", "已锁定", "时间设置",
                 "删除本书", "跳转到", "锁屏展示", "背光"}
VERBS = {"设置", "书库", "选择", "移动", "翻页", "打开", "确定", "返回",
         "重试", "刷新", "取消", "恢复", "长按", "解锁", "调整", "下一步",
         "跳转", "删除"}
VERBS_WIDE = {"下一步"}          # 3 个全角字 = 48 px
# 整页消息的逐行预算（COPY_DECK §2.8/§2.10）：原因行 <=112，动作行 <=96。
MSG_REASONS = {"原厂固件不存在", "分区表不匹配", "未开放此功能", "写入失败",
               "校验失败", "画面停在上一帧", "读不到内置存储", "没有书",
               "用网页上传", "需重新上传", "未上传锁屏图", "电量耗尽"}
MSG_ACTIONS = {"先刷原厂固件", "保持当前固件", "重启后重试", "重启设备",
               "已切换", "3秒后重启", "重启后生效",
               "设置完成", "请充电"}


def width(s):
    """COPY_DECK §1.5：ASCII 8 px，非 ASCII 16 px。"""
    return sum(8 if ord(ch) < 0x80 else 16 for ch in s)


def literals(path):
    """Yield (line, literal, source_line). Comments are skipped, # lines too."""
    src = open(path, encoding="utf-8").read()
    i = 0
    line = 1
    while i < len(src):
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = len(src) if j < 0 else j + 2
            line += src.count("\n", i, j)
            i = j
        elif src.startswith("//", i):
            j = src.find("\n", i)
            j = len(src) if j < 0 else j + 1
            line += src.count("\n", i, j)
            i = j
        elif src[i] == '"':
            j = i + 1
            buf = []
            while j < len(src) and src[j] != '"':
                if src[j] == "\\":
                    buf.append(src[j])
                    buf.append(src[j + 1])
                    j += 2
                else:
                    buf.append(src[j])
                    j += 1
            src_line = src[src.rfind("\n", 0, i) + 1:src.find("\n", i)]
            yield line, "".join(buf), src_line.strip()
            line += src.count("\n", i, j)
            i = j + 1
        else:
            if src[i] == "\n":
                line += 1
            i += 1


UI_FILES = {"main/mp_ui.c", "main/mp_hints.c", "main/mp_app.c"}
ASCII_SCREEN = {"Miaopaper E2S", "Miaopaper E2"}

bad = 0
count = 0
for path in files:
    rel = os.path.relpath(path, root)
    for ln, lit, src_line in literals(path):
        if src_line.startswith("#include") or src_line.startswith("_Static_assert"):
            continue
        count += 1
        for w in FORBIDDEN:
            if w in lit and lit not in FORBIDDEN_ALLOW:
                print("%s:%d: forbidden word %r in %r" % (rel, ln, w, lit))
                bad = 1
        m = KEYNAMES.search(lit)
        if m:
            print("%s:%d: key name %r in %r" % (rel, ln, m.group(1), lit))
            bad = 1
        for w in SD_WORDS:
            if w in lit:
                print("%s:%d: SD wording %r in %r" % (rel, ln, w, lit))
                bad = 1
        # 宽度：只量 UI 文件里的屏显串（含 CJK 或白名单英文），跳过格式串与
        # 日志（日志是 ASCII；mp_ble/main 的 CJK 日志不在 UI 文件里）。
        if "%" not in lit and rel in UI_FILES and (
                any(ord(ch) >= 0x80 for ch in lit) or lit in ASCII_SCREEN):
            w = width(lit)
            limit = 234
            if lit in STATUS_TITLES:
                limit = 112
            elif lit in MSG_REASONS:
                limit = 112
            elif lit in MSG_ACTIONS:
                limit = 96
            elif lit in VERBS_WIDE:
                limit = 48
            elif lit in VERBS:
                limit = 32
            if w > limit:
                print("%s:%d: width %d > %d for %r" % (rel, ln, w, limit, lit))
                bad = 1

if bad:
    print("copy: FAIL")
    sys.exit(1)
print("copy: %d literals OK (%d files)" % (count, len(files)))
PY
