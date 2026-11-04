#!/bin/sh
# Cross-compile on Linux/WSL with MinGW-w64 (Ubuntu 24.04: apt install g++-mingw-w64-x86-64-win32).
# The same script is used by GitHub Actions (.github/workflows/build.yml) for the release builds.
set -e
VK_HEADERS_TAG=v1.4.367
CXX=${CXX:-x86_64-w64-mingw32-g++-win32}
[ -d Vulkan-Headers ] || git clone --depth 1 --branch "$VK_HEADERS_TAG" https://github.com/KhronosGroup/Vulkan-Headers.git
# -s strips at link time and --no-insert-timestamp keeps the PE header free of the build time,
# so the same source and toolchain give a byte-identical DLL
"$CXX" -O2 -std=c++17 -shared -static -static-libgcc -static-libstdc++ -s -Wl,--no-insert-timestamp \
  -IVulkan-Headers/include src/xp_optimus_bypass.cpp -o dist/xp_optimus_bypass.dll -luser32 -lversion -ladvapi32
echo "built dist/xp_optimus_bypass.dll"
