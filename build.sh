#!/usr/bin/env bash
# Build sw2bridge firmware (WSL). Output: build/sw2bridge.uf2
#
# 一次性环境准备(无系统 gcc 的干净 WSL):
#   1. mise use -g cmake ninja python zig@0.14.1
#   2. zig 包装成宿主编译器(编译 pioasm/elf2uf2 等 SDK 宿主工具):
#        Z=$(mise where zig)/zig; mkdir -p ~/tools/hostbin
#        for p in cc:cc c++:c++ ar:ar ranlib:ranlib objcopy:objcopy; do \
#          printf '#!/bin/sh\nexec %s %s "$@"\n' "$Z" "${p##*:}" > ~/tools/hostbin/${p%%:*}; chmod +x ~/tools/hostbin/${p%%:*}; done
#   3. 预装 picotool(SDK 官方推荐;另可绕过 zig 下 cmake 无法判定 ELF
#      导致 picotool ExternalProject 的 Ninja RPATH 报错,见下 PICOTOOL_FETCH_FROM_GIT_PATH):
#        cmake -S build/_deps/picotool-src -B ~/tools/picotool-build -GNinja \
#          -DPICO_SDK_PATH="$PWD/pico-sdk" -DPICOTOOL_NO_LIBUSB=1 -DPICOTOOL_FLAT_INSTALL=1 \
#          -DCMAKE_INSTALL_PREFIX="$HOME/tools/picotool-install" -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON
#        cmake --build ~/tools/picotool-build && cmake --install ~/tools/picotool-build
set -e
cd "$(dirname "$0")"
export PICO_SDK_PATH="$PWD/pico-sdk"
export PATH="$HOME/tools/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/bin:$PATH"

# 宿主工具链:zig 包装脚本(见上“一次性环境准备”第 2 步)
export PATH="$HOME/tools/hostbin:$PATH"
export CC=cc CXX=c++   # 仅影响 pioasm/elf2uf2 等宿主工具;交叉编译器由 SDK 工具链文件强制指定

# 使用预装的 picotool(第 3 步),避免 SDK 现场 ExternalProject 构建
export PICOTOOL_FETCH_FROM_GIT_PATH="$HOME/tools/picotool-install"

cmake -S firmware -B build -GNinja
cmake --build build
echo
echo "== uf2: $(pwd)/build/sw2bridge.uf2 =="
