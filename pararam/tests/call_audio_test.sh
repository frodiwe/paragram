#!/bin/bash
# End to end test of call audio: two Pararam Desktop clients, Alice and
# Carol, call each other through pararam/mock_server.py on a virtual
# display. Each client has its own PulseAudio server: the microphone plays a
# tone (Alice 440 Hz, Carol 660 Hz) and the speaker is recorded. The test
# checks that the media server gets each tone and that each speaker plays
# the tone of the other side.
#
# Needs Xvfb, xdotool, PulseAudio (pulseaudio, pactl, pacat, parec) and a
# Python with aiohttp, aiortc and numpy for the mock (see pararam/README.md).
#
# Usage: PYTHON=/path/to/venv/bin/python \
#   pararam/tests/call_audio_test.sh /path/to/Telegram [qt lib dir]
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
APP=${1:?Pass the Pararam Desktop binary}
QT_LIB=${2:-}
PYTHON=${PYTHON:-python3}
OUT=${OUT:-/tmp/pararam-call-audio}
PORT=${PORT:-8795}
CALLS_PORT=$((PORT + 1))
export DISPLAY=${TEST_DISPLAY:-:98}
TOOLS="$ROOT/pararam/tests/call_audio_tools.py"
API=http://127.0.0.1:$PORT

PIDS=()
cleanup() {
	for pid in "${PIDS[@]}"; do
		kill "$pid" 2> /dev/null || true
	done
	pkill -f "pulseaudio.*$OUT" 2> /dev/null || true
}
trap cleanup EXIT

rm -rf "$OUT"
mkdir -p "$OUT"

fail() {
	echo "FAIL: $*"
	exit 1
}

# Audio: a server per client with a "speaker" and a tone as the microphone.
audio() {
	local name=$1 frequency=$2 dir="$OUT/pulse-$1"
	mkdir -p "$dir"
	PULSE_RUNTIME_PATH="$dir" PULSE_STATE_PATH="$dir" HOME="$dir" \
		pulseaudio -n --daemonize=no --exit-idle-time=-1 --use-pid-file=no \
		--log-target=file:"$dir/pulse.log" \
		-L "module-native-protocol-unix socket=$dir/native auth-anonymous=1" \
		-L "module-null-sink sink_name=speaker" \
		-L "module-null-sink sink_name=microphone" \
		> /dev/null 2>&1 &
	PIDS+=($!)
	for _ in $(seq 1 40); do
		[ -S "$dir/native" ] && break
		sleep 0.25
	done
	local server="unix:$dir/native"
	pactl -s "$server" set-default-sink speaker
	pactl -s "$server" set-default-source microphone.monitor
	"$PYTHON" "$TOOLS" tone "$frequency" | pacat -s "$server" \
		-d microphone --raw --format=s16le --rate=48000 --channels=1 &
	PIDS+=($!)
}
audio alice 440
audio carol 660

Xvfb "$DISPLAY" -screen 0 2600x820x24 > "$OUT/xvfb.log" 2>&1 &
PIDS+=($!)

"$PYTHON" -u "$ROOT/pararam/mock_server.py" \
	--port "$PORT" \
	--calls-port "$CALLS_PORT" > "$OUT/mock.log" 2>&1 &
PIDS+=($!)
for _ in $(seq 1 60); do
	curl -s -o /dev/null -X POST "$API/_mock/state" -d '{}' && break
	sleep 1
done
# Carol answers herself.
curl -s -X POST "$API/_mock/call_config" -d '{"auto_answer": false}' \
	> /dev/null || fail "mock server did not start"

# Clients, side by side: Alice at x = 0, Carol at x = 1300.
windows() {
	xdotool search --onlyvisible --name "Pararam" 2> /dev/null | sort
}
client() {
	local name=$1 x=$2 email=$3 before after window
	before=$(windows)
	env PULSE_SERVER="unix:$OUT/pulse-$name/native" \
		PARARAM_API_URL=$API \
		PARARAM_FILE_URL=$API \
		PARARAM_SOCK_URL=ws://127.0.0.1:$CALLS_PORT \
		PARARAM_MEDIA_INSECURE=1 \
		PARARAM_CALL_ALLOW_DIRECT=1 \
		PARARAM_CALL_ANY_CODEC=1 \
		QT_QPA_PLATFORM=xcb \
		LD_LIBRARY_PATH="$QT_LIB" \
		"$APP" -workdir "$OUT/client-$name" > "$OUT/client-$name.log" 2>&1 &
	PIDS+=($!)
	for _ in $(seq 1 90); do
		after=$(windows)
		window=$(comm -13 <(echo "$before") <(echo "$after") | head -1)
		[ -n "$window" ] && break
		sleep 1
	done
	[ -n "$window" ] || fail "no window of $name"
	sleep 3
	xdotool windowsize "$window" 1280 800 windowmove "$window" "$x" 0
	sleep 1
	xdotool mousemove $((x + 640)) 525 click 1 # Start Messaging.
	sleep 2
	xdotool mousemove $((x + 640)) 345 click 1
	xdotool type --delay 30 "$email"
	xdotool mousemove $((x + 640)) 400 click 1
	xdotool type --delay 30 password
	xdotool key Return
	sleep 8
}
client alice 0 alice@example.com
client carol 1300 carol@example.com

state() {
	curl -s -X POST "$API/_mock/call_state" -d '{}'
}
member() { # member USER FIELD...: a value of the room member from call_state.
	state | "$PYTHON" -c "
import json, sys
value = None
for room in json.load(sys.stdin).values():
    value = room['members'].get('$1')
    if value is not None:
        break
for key in sys.argv[1:]:
    value = (value or {}).get(key)
print(json.dumps(value))" "${@:2}"
}

# Alice calls Carol from their private chat, Carol accepts.
xdotool mousemove 290 180 click 1 # The chat with Carol.
sleep 3
xdotool mousemove 880 58 click 1 # Call.
sleep 3
xdotool mousemove 708 616 click 1 # Start Call.
for _ in $(seq 1 20); do
	[ "$(member 1 audio)" != "null" ] && break
	sleep 1
done
[ "$(member 1 audio)" != "null" ] || fail "Alice did not join the call"
sleep 3
xdotool mousemove 1332 626 click 1 # Carol: Accept.
for _ in $(seq 1 30); do
	[ "$(member 3 audio)" != "null" ] && break
	sleep 1
done
[ "$(member 3 audio)" != "null" ] || fail "Carol did not join the call"

# Let the audio flow, then measure both directions.
sleep 10
import -window root "$OUT/call.png" 2> /dev/null || true
check() { # check WHAT JSON FREQUENCY
	"$PYTHON" -c "
import json, sys
value = json.loads(sys.argv[2])
ok = value['rms'] > 300 and abs(value['frequency'] - $3) < 20
print('%-26s rms %5d, %4d Hz (want %d Hz): %s' % (
    sys.argv[1], value['rms'], value['frequency'], $3,
    'ok' if ok else 'FAIL'))
sys.exit(0 if ok else 1)" "$1" "$2"
}
result=0
check "server gets Alice" "$(member 1 sent_audio)" 440 || result=1
check "server gets Carol" "$(member 3 sent_audio)" 660 || result=1
check "Alice hears" "$("$PYTHON" "$TOOLS" listen \
	"unix:$OUT/pulse-alice/native" 3)" 660 || result=1
check "Carol hears" "$("$PYTHON" "$TOOLS" listen \
	"unix:$OUT/pulse-carol/native" 3)" 440 || result=1
if [ $result = 0 ]; then
	echo "ALL OK"
else
	echo "FAILED, see $OUT"
fi
exit $result
