#!/usr/bin/env bash
# Builds build/instanterchat.dll on Linux with clang-cl and lld-link, as a 32-bit Ashita v4 plugin.
# Uses the shared toolchain (default ../.toolchain): ashita/plugins/sdk and an xwin msvc splat.
# ASHITA_SDK_PATH and MSVC_PATH override either.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLCHAIN="${TOOLCHAIN:-$HERE/../.toolchain}"
SDK="${ASHITA_SDK_PATH:-$TOOLCHAIN/ashita/plugins/sdk}"
MSVC="${MSVC_PATH:-$TOOLCHAIN/msvc}"
OUT="$HERE/build"

die() { echo "build: $*" >&2; exit 1; }
[[ -f "$SDK/Ashita.h" ]] || die "no Ashita.h in $SDK"
[[ -d "$MSVC/crt/include" && -d "$MSVC/sdk/include/um" ]] || die "no C runtime and Windows SDK in $MSVC"

mkdir -p "$OUT/lib"

clang-cl --target=i686-pc-windows-msvc /nologo /c /std:c++latest /O2 /MD /EHa /W4 /permissive- \
    /DWIN32_LEAN_AND_MEAN /DNDEBUG /D_CRT_SECURE_NO_WARNINGS \
    /imsvc "$MSVC/crt/include" /imsvc "$MSVC/sdk/include/ucrt" \
    /imsvc "$MSVC/sdk/include/um" /imsvc "$MSVC/sdk/include/shared" /imsvc "$SDK" \
    "/Fo$OUT/instanterchat.obj" "$HERE/src/instanterchat.cpp"

# The Ashita SDK asks for Psapi.lib by a spelling the Windows SDK does not have on a
# case-sensitive file system
ln -sf "$MSVC/sdk/lib/um/x86/psapi.lib" "$OUT/lib/Psapi.lib"

lld-link /nologo /dll /machine:x86 /opt:ref /opt:icf "/def:$HERE/src/exports.def" "/out:$OUT/instanterchat.dll" \
    "/libpath:$MSVC/crt/lib/x86" "/libpath:$MSVC/sdk/lib/ucrt/x86" "/libpath:$MSVC/sdk/lib/um/x86" "/libpath:$OUT/lib" \
    "$OUT/instanterchat.obj" psapi.lib

echo "built $OUT/instanterchat.dll"
