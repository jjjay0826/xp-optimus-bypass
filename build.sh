#!/bin/sh
# Cross-compile on Linux/WSL with MinGW-w64 (Debian/Ubuntu: apt install g++-mingw-w64-x86-64).
set -e
[ -d Vulkan-Headers ] || git clone --depth 1 https://github.com/KhronosGroup/Vulkan-Headers.git
x86_64-w64-mingw32-g++ -O2 -std=c++17 -shared -static -static-libgcc -static-libstdc++ \
  -IVulkan-Headers/include src/xp_optimus_bypass.cpp -o dist/xp_optimus_bypass.dll -luser32
x86_64-w64-mingw32-strip dist/xp_optimus_bypass.dll
echo "built dist/xp_optimus_bypass.dll"
