#!/bin/bash
# Builds pararam/tests/backend_test.cpp with the compiler flags of an
# existing Linux CMake build of Pararam Desktop and runs it against
# pararam/mock_server.py.
#
# Usage: pararam/tests/run_backend_test.sh /path/to/cmake/build/dir
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=$(cd "${1:?Pass the CMake build directory}" && pwd)
OUT=${OUT:-/tmp/pararam-backend-test}
PORT=${PORT:-8790}
mkdir -p "$OUT"
cd "$BUILD"

TARGET=Telegram/CMakeFiles/Telegram.dir/SourceFiles/pararam/pararam_backend.cpp.o
COMMAND=$(ninja -t commands "$TARGET" | tail -1 \
	| sed -e 's| -MD -MT [^ ]* -MF [^ ]*||' -e 's| -o [^ ]* -c [^ ]*$||')

compile() {
	echo "Compiling $(basename "$1")"
	eval "$COMMAND -Wno-deprecated -o '$2' -c '$1'"
}
compile "$ROOT/pararam/tests/backend_test.cpp" "$OUT/backend_test.o"
compile "$ROOT/Telegram/SourceFiles/pararam/pararam_backend.cpp" "$OUT/backend.o"
compile "$ROOT/Telegram/SourceFiles/pararam/pararam_api.cpp" "$OUT/api.o"
compile "$ROOT/Telegram/SourceFiles/pararam/pararam_data.cpp" "$OUT/data.o"
compile "$ROOT/Telegram/SourceFiles/pararam/pararam_calls.cpp" "$OUT/calls.o"
compile "$ROOT/Telegram/SourceFiles/pararam/pararam_call_session.cpp" "$OUT/call_session.o"
compile "$ROOT/Telegram/SourceFiles/pararam/pararam_call_signaling.cpp" "$OUT/call_signaling.o"
compile "$ROOT/Telegram/SourceFiles/pararam/pararam_websocket.cpp" "$OUT/websocket.o"

QT=$(grep -m1 "Qt6Core_DIR:PATH=" CMakeCache.txt | sed 's|.*=||; s|/lib/cmake/Qt6Core||')
echo "Linking"
c++ -o "$OUT/backend_test" \
	"$OUT/backend_test.o" "$OUT/backend.o" "$OUT/api.o" "$OUT/data.o" \
	"$OUT/calls.o" "$OUT/call_session.o" "$OUT/call_signaling.o" \
	"$OUT/websocket.o" \
	Telegram/CMakeFiles/td_scheme.dir/gen/scheme.cpp.o \
	Telegram/CMakeFiles/td_scheme.dir/gen/scheme-dump_to_text.cpp.o \
	Telegram/CMakeFiles/td_mtproto.dir/SourceFiles/mtproto/details/mtproto_dump_to_text.cpp.o \
	Telegram/lib_tl/CMakeFiles/lib_tl.dir/tl/tl_basic_types.cpp.o \
	Telegram/CMakeFiles/td_mtproto.dir/SourceFiles/mtproto/mtproto_dh_utils.cpp.o \
	Telegram/lib_base/CMakeFiles/lib_base.dir/base/timer.cpp.o \
	Telegram/lib_base/CMakeFiles/lib_base.dir/base/random.cpp.o \
	Telegram/lib_base/CMakeFiles/lib_base.dir/base/bytes.cpp.o \
	Telegram/lib_base/CMakeFiles/lib_base.dir/base/concurrent_timer.cpp.o \
	Telegram/lib_crl/CMakeFiles/lib_crl.dir/crl/crl_time.cpp.o \
	Telegram/lib_crl/CMakeFiles/lib_crl.dir/crl/linux/crl_linux_time.cpp.o \
	Telegram/lib_crl/CMakeFiles/lib_crl.dir/crl/common/crl_common_on_main.cpp.o \
	Telegram/lib_crl/CMakeFiles/lib_crl.dir/crl/common/crl_common_queue.cpp.o \
	Telegram/lib_crl/CMakeFiles/lib_crl.dir/crl/common/crl_common_list.cpp.o \
	-L"$QT/lib" -Wl,-rpath,"$QT/lib" \
	-lQt6Gui -lQt6Network -lQt6Core -lcrypto -lz -lpthread

python3 "$ROOT/pararam/mock_server.py" --port "$PORT" > "$OUT/mock.log" 2>&1 &
MOCK=$!
trap 'kill $MOCK' EXIT
sleep 1
PARARAM_API_URL=http://127.0.0.1:$PORT \
PARARAM_FILE_URL=http://127.0.0.1:$PORT \
	"$OUT/backend_test"
