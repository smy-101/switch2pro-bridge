#!/usr/bin/env bash
# Host-side regression tests: no board required.
set -e
cd "$(dirname "$0")/.."
CC_BIN="${CC:-cc}"
command -v "$CC_BIN" >/dev/null 2>&1 || CC_BIN="$HOME/tools/hostbin/cc"
mkdir -p build
"$CC_BIN" -I firmware -Wall -Wextra \
    tests/test_stick_cal.c firmware/pro.c firmware/sw2_decode.c \
    -o build/test_stick_cal
exec build/test_stick_cal
