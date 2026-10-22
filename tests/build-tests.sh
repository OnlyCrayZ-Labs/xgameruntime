#!/bin/sh
# MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md.
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eu
# Run from the Proton repository root inside its configured SDK container.
root=$(pwd)
test -d "$root/wine/dlls/xgameruntime"
for arch in x86_64 i386; do
    cc=x86_64-w64-mingw32-gcc
    cxx=x86_64-w64-mingw32-g++
    if [ "$arch" = i386 ]; then cc=i686-w64-mingw32-gcc; cxx=i686-w64-mingw32-g++; fi
    "$cc" -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter -D__WINESRC__ \
        -I"build/build-msfs-xodus/obj-wine-$arch/include" -I"wine/include" -I"wine/dlls/xgameruntime" \
        -I"build/build-msfs-xodus/obj-wine-$arch/dlls/xgameruntime" \
        wine/dlls/xgameruntime/tests/taskqueue.c -o "build/xgameruntime/taskqueue-tests-$arch.exe" -lole32 -luuid
    "$cc" -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter -D__WINESRC__ \
        -I"build/build-msfs-xodus/obj-wine-$arch/include" -I"wine/include" -I"wine/dlls/xgameruntime" \
        -I"build/build-msfs-xodus/obj-wine-$arch/dlls/xgameruntime" \
        wine/dlls/xgameruntime/tests/user.c -o "build/xgameruntime/user-tests-$arch.exe" -lole32 -luuid
    "$cc" -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter -D__WINESRC__ \
        -I"build/build-msfs-xodus/obj-wine-$arch/include" -I"wine/include" -I"wine/dlls/xgameruntime" \
        -I"build/build-msfs-xodus/obj-wine-$arch/dlls/xgameruntime" \
        wine/dlls/xgameruntime/tests/networking.c -o "build/xgameruntime/networking-tests-$arch.exe" -lole32 -luuid
    "$cc" -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter -D__WINESRC__ \
        -I"build/build-msfs-xodus/obj-wine-$arch/include" -I"wine/include" -I"wine/dlls/xgameruntime" \
        -I"build/build-msfs-xodus/obj-wine-$arch/dlls/xgameruntime" \
        wine/dlls/xgameruntime/tests/registration.c -o "build/xgameruntime/registration-tests-$arch.exe" -lole32 -luuid
    "$cc" -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter -D__WINESRC__ \
        -I"build/build-msfs-xodus/obj-wine-$arch/include" -I"wine/include" -I"wine/dlls/xgameruntime" \
        -I"build/build-msfs-xodus/obj-wine-$arch/dlls/xgameruntime" \
        wine/dlls/xgameruntime/tests/identity.c -o "build/xgameruntime/identity-tests-$arch.exe" -lole32 -luuid
    "$cc" -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter -D__WINESRC__ \
        -I"build/build-msfs-xodus/obj-wine-$arch/include" -I"wine/include" -I"wine/dlls/xgameruntime" \
        -I"build/build-msfs-xodus/obj-wine-$arch/dlls/xgameruntime" \
        wine/dlls/xgameruntime/tests/xbl.c -o "build/xgameruntime/xbl-tests-$arch.exe" -lole32 -luuid

    "$cxx" -std=c++11 -Wall -Wextra -Werror \
        -I"wine/dlls/xgameruntime" \
        -I"build/build-msfs-xodus/obj-wine-$arch/dlls/xgameruntime" \
        -c wine/dlls/xgameruntime/tests/networking-abi.cpp -o "build/xgameruntime/networking-abi-$arch.o"
 done
