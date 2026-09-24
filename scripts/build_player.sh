#!/bin/sh
# Build komi-player (native/player) in the same tree as the browser EBOOT.
# Output: vendor/tilefinch/build-preset-psp/komi-player/{komi-player.prx,EBOOT.PBP,...}
set -eu
PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$PROJECT_ROOT/scripts/env.sh"
cd "$PROJECT_ROOT/vendor/tilefinch"
cmake --preset psp -DPython3_EXECUTABLE="$PROJECT_ROOT/.venv/bin/python" \
    -DCMAKE_AR="$PSPDEV/bin/psp-ar" -DCMAKE_RANLIB="$PSPDEV/bin/psp-ranlib" \
    -DCMAKE_NM="$PSPDEV/bin/psp-nm" -DCMAKE_STRIP="$PSPDEV/bin/psp-strip" \
    -DCMAKE_LINKER="$PSPDEV/bin/psp-ld" -DCMAKE_OBJDUMP="$PSPDEV/bin/psp-objdump" \
    -DCMAKE_C_FLAGS_MINSIZEREL="-Os -DNDEBUG" \
    -DKOMI_EXTRA_CMAKE="$PROJECT_ROOT/native/player/player.cmake" > /dev/null
cmake --build build-preset-psp --target komi-player-prx komi-player-elf -j8
echo "komi-player: $PROJECT_ROOT/vendor/tilefinch/build-preset-psp/komi-player"
