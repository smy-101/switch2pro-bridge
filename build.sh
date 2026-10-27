#!/usr/bin/env bash
# Build sw2bridge firmware (WSL). Output: build/sw2bridge.uf2
set -e
cd "$(dirname "$0")"
export PICO_SDK_PATH="$PWD/pico-sdk"
export PATH="$HOME/tools/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/bin:$PATH"
cmake -S firmware -B build -GNinja
cmake --build build
echo
echo "== uf2: $(pwd)/build/sw2bridge.uf2 =="
