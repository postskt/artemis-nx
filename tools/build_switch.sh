#!/usr/bin/env bash
# Build the Nintendo Switch .nro from an MSYS2 shell.
set -euo pipefail

export PATH="/usr/bin:/bin:$PATH"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source /etc/profile.d/devkit-env.sh
export PATH="/opt/devkitpro/tools/bin:/opt/devkitpro/devkitA64/bin:/opt/devkitpro/portlibs/switch/bin:$PATH"

cmake -S "$ROOT/cpp_runner" -B "$ROOT/cpp_runner/build-switch" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DARTC_BUILD_SMOKE=OFF \
  -DARTC_BUILD_PLATFORM=ON
cmake --build "$ROOT/cpp_runner/build-switch" -j "$(nproc)"

# Keep the staged NSP ExeFS in sync with the freshly linked ELF.
mkdir -p "$ROOT/output/nsp_build/exefs"
/opt/devkitpro/tools/bin/elf2nso \
  "$ROOT/cpp_runner/build-switch/src/platform/artemis_runner.elf" \
  "$ROOT/output/nsp_build/exefs/main"

echo "NRO: $ROOT/cpp_runner/build-switch/artemis_runner.nro"
echo "NSO: $ROOT/output/nsp_build/exefs/main"
