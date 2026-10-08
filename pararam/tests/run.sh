#!/bin/bash
# Builds and runs the standalone pararam data tests on Linux.
# Usage: QT=/path/to/Qt/6.x/gcc_64 pararam/tests/run.sh
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
QT=${QT:?Set QT to the Qt installation prefix}
OUT=${OUT:-/tmp/pararam-tests}
mkdir -p "$OUT"
"$QT/libexec/moc" -o "$OUT/moc_api.cpp" "$ROOT/Telegram/SourceFiles/pararam/pararam_api.h" 2>/dev/null || true
g++ -std=c++20 -fPIC -O0 -g -Wno-deprecated \
	-I"$ROOT/Telegram/SourceFiles" \
	-I"$ROOT/Telegram/lib_base" \
	-I"$ROOT/Telegram/ThirdParty/GSL/include" \
	-I"$ROOT/Telegram/ThirdParty/range-v3/include" \
	-I"$ROOT/Telegram/ThirdParty/expected/include" \
	-I"$QT/include" -I"$QT/include/QtCore" -I"$QT/include/QtNetwork" \
	"$ROOT/pararam/tests/pararam_data_test.cpp" \
	"$ROOT/Telegram/SourceFiles/pararam/pararam_data.cpp" \
	"$ROOT/Telegram/SourceFiles/pararam/pararam_api.cpp" \
	-L"$QT/lib" -Wl,-rpath,"$QT/lib" -lQt6Core -lQt6Network \
	-o "$OUT/pararam_data_test"
"$OUT/pararam_data_test"
