#!/usr/bin/env bash
# Build e2s_mp (app-only). This script never flashes, erases or reads the
# device.
#
#   ./tools/build.sh                 # idf.py build
#   ./tools/build.sh size            # idf.py size
#   ./tools/build.sh fullclean build # explicit IDF subcommands
#
# e2s_mp is an app-only image: the bootloader (0x0), partition table (0x8000)
# and otadata (0xF000) are off limits. The only otadata writer in the firmware
# is recovery_stock_commit() (PORT_SPEC §8.1, docs/research/platform.md
# §11#1-4); this script must not be a second one. It refuses the
# device-writing idf.py subcommands outright, anywhere in the argument list
# (idf.py accepts options before the verb and allows chaining verbs, so
# checking only "$1" is not enough). App-only flashing is a separate manual
# step: tools/flash_app.sh only PRINTS the esptool command for
# 0x20000/0x210000 and never touches the device; never "idf.py flash".
set -euo pipefail

# ROOT is derived from this script's own location, so it always points at
# e2s_mp (this file lives in e2s_mp/tools/). Verify instead of trusting cwd.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IDF="${IDF_PATH:-$HOME/esp/esp-idf-v6.1.1}"

if ! grep -q '^project(miaopaper_e2_e2s' "$ROOT/CMakeLists.txt" 2>/dev/null; then
    echo "build.sh: $ROOT is not the miaopaper_e2_e2s project (no project(miaopaper_e2_e2s) in CMakeLists.txt)" >&2
    exit 2
fi

if [ ! -f "$IDF/export.sh" ]; then
    echo "build.sh: ESP-IDF not found at $IDF (set IDF_PATH)" >&2
    exit 2
fi

if [ "$#" -eq 0 ]; then
    set -- build
fi

for arg in "$@"; do
    case "$arg" in
        flash|app-flash|bootloader-flash|partition-table-flash|otadata-flash|encrypted-flash|erase-flash|erase-otadata|read-otadata|dfu|dfu-flash|uf2)
            echo "build.sh: refusing '$arg' — e2s_mp is app-only (PORT_SPEC §8.1/§10.5);" \
                 "otadata is written only by recovery_stock_commit()" >&2
            exit 3
            ;;
    esac
done

# shellcheck disable=SC1090
. "$IDF/export.sh" >/dev/null

cd "$ROOT"
exec idf.py "$@"
