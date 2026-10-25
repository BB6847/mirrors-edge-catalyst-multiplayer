#!/bin/bash
# Compile mec_overlay.dll (Windows x64) avec clang.
#
#   Sous Windows : installe llvm-mingw (https://github.com/mstorsjo/llvm-mingw), puis dans un terminal
#   bash (Git Bash / MSYS2) :  ./build.sh
#   Sous Linux : avec clang + lld, et soit llvm-mingw dans le PATH, soit SYSROOT=<dossier avec include/ et
#   lib/ de mingw-w64> ./build.sh
#
# Résultat : overlay/build/mec_overlay.dll (à poser à côté de coop.py).
set -e
cd "$(dirname "$0")"
HERE=$(pwd)
I=$HERE/third_party/imgui
M=$HERE/third_party/minhook
SRC=$HERE/src
OUT=$HERE/build
mkdir -p "$OUT"
[ -f "$I/imgui.cpp" ] || { echo "ImGui absent de third_party/imgui"; exit 1; }

TARGET=--target=x86_64-w64-windows-gnu
SYS=""
LIBS=""
if [ -n "$SYSROOT" ]; then
  SYS="-nostdlibinc -isystem $SYSROOT/include"
  LIBS="-L$SYSROOT/lib"
fi
CF="$TARGET $SYS -O2 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-asynchronous-unwind-tables \
 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
 -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS -DIMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS \
 -DIMGUI_DISABLE_FILE_FUNCTIONS -D__USE_MINGW_ANSI_STDIO=0 -DNDEBUG"

cd "$OUT"
# bibliothèques (ImGui, MinHook)
for f in $I/imgui.cpp $I/imgui_draw.cpp $I/imgui_tables.cpp $I/imgui_widgets.cpp \
         $I/backends/imgui_impl_dx11.cpp $I/backends/imgui_impl_win32.cpp; do
  clang++ $CF -w -std=c++17 -I$I -c "$f" -o "$(basename "$f" .cpp).o"
done
for f in $M/src/buffer.c $M/src/hook.c $M/src/trampoline.c $M/src/hde/hde64.c; do
  clang $CF -w -I$M/include -c "$f" -o "$(basename "$f" .c).o"
done
# le mod
OBJS=""
for f in overlay game_core ghosts models sync mission audio research research_ai combat menu progression interact aitarget; do
  if [ "$f" = overlay ]; then EXTRA="-I$I -I$M/include"; else EXTRA=""; fi
  clang++ $CF -std=c++17 $EXTRA -c "$SRC/$f.cpp" -o "$f.o"
  OBJS="$OBJS $f.o"
done
clang $CF -c "$SRC/shim.c" -o shim.o
clang $TARGET -c "$SRC/chkstk.S" -o chkstk.o
clang++ $TARGET -fuse-ld=lld -shared -nostdlib -o mec_overlay.dll \
  $OBJS shim.o chkstk.o imgui.o imgui_draw.o imgui_tables.o imgui_widgets.o imgui_impl_dx11.o imgui_impl_win32.o \
  buffer.o hook.o trampoline.o hde64.o \
  $LIBS -lkernel32 -luser32 -lgdi32 -ld3d11 -ldxgi -ld3dcompiler_47 -ldwmapi -lmsvcrt \
  -Wl,--entry=DllMain -Wl,--subsystem,windows
echo "OK : $OUT/mec_overlay.dll"
