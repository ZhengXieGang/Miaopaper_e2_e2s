#!/bin/sh
# sim/run.sh — build the host simulator, render frames, make PNGs.
#
#   sim/run.sh --all --out DIR    build + render all fixtures + contact sheet
#   sim/run.sh --one clock --out D   single fixture (debug)
#   sim/run.sh --check            build + frame invariants (+ copy lint if present)
#   sim/run.sh --all --no-build   reuse sim/build/sim_mp
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
NO_BUILD=0
UI_MODE=auto
HINTS_MODE=auto

usage() {
    cat <<'EOF'
usage: sim/run.sh [--all] [--one NAME] [--check] [--out DIR] [--no-build]
                  [--ui auto|real|dummy] [--hints auto|real|dummy]

  --all            render every fixture (default) and build the contact sheet
  --one NAME       render one fixture only (see --list in the binary)
  --check          validate frame invariants, write no files
  --out DIR        output directory (default: sim/out)
  --no-build       do not run make (use the existing sim/build/sim_mp)
  --ui KIND        real (main/mp_ui.c) or dummy (sim/stub_ui.c)
  --hints KIND     real (main/mp_hints.c) or dummy (sim/stub_hints.c)
  -h, --help       this text

Environment: MP_SIM_ASSETS=/path/to/assets, CC=<compiler>
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
