#!/bin/sh
# sim/run.sh — build the host simulator, render frames, make PNGs.
#
#   sim/run.sh --all --out DIR       build + render all fixtures + contact sheet
#   sim/run.sh --one clock --out D   single fixture (debug)
#   sim/run.sh --check               UI 帧断言 + 文案 lint + 全链路固件回归 (ASan/UBSan)
#   sim/run.sh --regress [--only ID] 仅运行本机全链路固件回归测试
#   sim/run.sh --all --no-build      reuse sim/build/sim_mp
#
# Environment:
#   MP_SIM_ASSETS=/path/to/assets   override the font asset directory
#   CC=gcc                          compiler (must accept -std=c11)
#
# The binary writes <out>/pgm/NAME.pgm; make_contact.py turns those into
# NAME.png (3x + title), NAME_1x.png and contact_sheet.png.
set -eu

SIM_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SIM_DIR/.." && pwd)
OUT_DIR="$SIM_DIR/out"
MODE=all
ONE=""
ONLY=""
NO_BUILD=0
UI_MODE=auto
HINTS_MODE=auto

usage() {
    cat <<'EOF'
usage: sim/run.sh [--all] [--one NAME] [--check] [--regress [--only ID]]
                  [--out DIR] [--no-build]
                  [--ui auto|real|dummy] [--hints auto|real|dummy]

  --all            render every fixture (default) and build the contact sheet
  --one NAME       render one fixture only (see --list in the binary)
  --check          validate UI frames + copy lint + full firmware regression
  --regress        build and run host firmware regression suite (ASan/UBSan)
  --only ID        run a single firmware regression case (with --regress, e.g. T11)
  --out DIR        output directory (default: sim/out)
  --no-build       do not run make (use existing binaries in sim/build)
  --ui KIND        real (main/mp_ui.c) or dummy (sim/stub_ui.c)
  --hints KIND     real (main/mp_hints.c) or dummy (sim/stub_hints.c)
  -h, --help       this text

Environment: MP_SIM_ASSETS=/path/to/assets, MP_SIM_OTA_BIN=/path/to/app.bin,
             SANITIZE=1|0, CC=<compiler>
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --all)
            MODE=all
            ;;
        --one)
            if [ $# -lt 2 ]; then
                echo "run.sh: --one needs a fixture name" >&2
                exit 2
            fi
            MODE=one
            ONE=$2
            shift
            ;;
        --out)
            if [ $# -lt 2 ]; then
                echo "run.sh: --out needs a directory" >&2
                exit 2
            fi
            OUT_DIR=$2
            shift
            ;;
        --check)
            MODE=check
            ;;
        --regress)
            MODE=regress
            ;;
        --only)
            if [ $# -lt 2 ]; then
                echo "run.sh: --only needs a regression case id (e.g. T11)" >&2
                exit 2
            fi
            ONLY=$2
            shift
            ;;
        --no-build)
            NO_BUILD=1
            ;;
        --ui)
            if [ $# -lt 2 ]; then
                echo "run.sh: --ui needs auto|real|dummy" >&2
                exit 2
            fi
            UI_MODE=$2
            shift
            ;;
        --hints)
            if [ $# -lt 2 ]; then
                echo "run.sh: --hints needs auto|real|dummy" >&2
                exit 2
            fi
            HINTS_MODE=$2
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "run.sh: unknown option '$1'" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

BIN="$SIM_DIR/build/sim_mp"
FW_BIN="$SIM_DIR/build/sim_fw_regress"

if [ "$MODE" = regress ]; then
    if [ "$NO_BUILD" -eq 0 ]; then
        make -C "$SIM_DIR" fw-regress
    elif [ ! -x "$FW_BIN" ]; then
        echo "run.sh: --no-build but $FW_BIN does not exist" >&2
        exit 2
    fi
    if [ -n "$ONLY" ]; then
        exec "$FW_BIN" --only "$ONLY"
    else
        exec "$FW_BIN"
    fi
fi

if [ "$NO_BUILD" -eq 0 ]; then
    make -C "$SIM_DIR" UI="$UI_MODE" HINTS="$HINTS_MODE"
elif [ ! -x "$BIN" ]; then
    echo "run.sh: --no-build but $BIN does not exist" >&2
    exit 2
fi

if [ "$MODE" = check ]; then
    "$BIN" --check
    if [ -f "$ROOT/tools/check_copy.sh" ]; then
        sh "$ROOT/tools/check_copy.sh"
    else
        echo "copy lint: tools/check_copy.sh is not present yet; skipped"
    fi
    if [ "$UI_MODE" != dummy ] && [ "$HINTS_MODE" != dummy ]; then
        if [ "$NO_BUILD" -eq 0 ]; then
            make -C "$SIM_DIR" fw-regress
        fi
        if [ -x "$FW_BIN" ]; then
            "$FW_BIN"
        fi
    fi
    exit 0
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "run.sh: python3 not found (needed by make_contact.py)" >&2
    exit 2
fi

TAG=""
if [ "$UI_MODE" = dummy ] || [ "$HINTS_MODE" = dummy ]; then
    TAG="dummy UI"
elif [ "$UI_MODE" = auto ] && [ ! -f "$ROOT/main/mp_ui.c" ]; then
    TAG="dummy UI (main/mp_ui.c missing)"
fi

if [ "$MODE" = one ]; then
    "$BIN" --one "$ONE" --out "$OUT_DIR"
    if [ -n "$TAG" ]; then
        python3 "$SIM_DIR/make_contact.py" --out "$OUT_DIR" --only "$ONE" --tag "$TAG"
    else
        python3 "$SIM_DIR/make_contact.py" --out "$OUT_DIR" --only "$ONE"
    fi
else
    "$BIN" --all --out "$OUT_DIR"
    if [ -n "$TAG" ]; then
        python3 "$SIM_DIR/make_contact.py" --out "$OUT_DIR" --tag "$TAG"
    else
        python3 "$SIM_DIR/make_contact.py" --out "$OUT_DIR"
    fi
fi

echo "run.sh: done -> $OUT_DIR"
