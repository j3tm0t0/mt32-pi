#!/bin/sh
#
# Real-time robustness test for mt32-pi on Linux.
#
# Plays a dense multi-channel workload (SoundFont by default, -y mt32 for MT-32) through the live audio path while
# stress-ng loads every CPU, and prints render-time/xrun statistics.
# Run it on the target (e.g. Raspberry Pi 4) with the real output device.
#
#   linux/scripts/loadtest.sh -b build-linux/mt32-pi -d ~/mt32-pi -D hw:0 -t 60
#
# Requires: stress-ng, python3.
#

set -eu

BIN=./build-linux/mt32-pi
DATA=.
DEVICE=default
SECS=60
SYNTH=soundfont
STRESS="--cpu 0 --vm 2 --vm-bytes 25% --io 1"

while getopts b:d:D:t:s:y: OPT; do
	case $OPT in
		b) BIN=$OPTARG ;;
		d) DATA=$OPTARG ;;
		D) DEVICE=$OPTARG ;;
		t) SECS=$OPTARG ;;
		s) STRESS=$OPTARG ;;
		y) SYNTH=$OPTARG ;;
		*) echo "usage: $0 [-b binary] [-d data-dir] [-D alsa-device] [-t seconds] [-s 'stress-ng args'] [-y mt32|soundfont]"; exit 1 ;;
	esac
done

HERE=$(cd "$(dirname "$0")/.." && pwd)
MID=$(mktemp --suffix=.mid)
STRESS_PID=
trap 'rm -f "$MID"; kill $STRESS_PID 2>/dev/null || true' EXIT

python3 "$HERE/tools/mkheavy.py" "$MID" 600

echo "== baseline (no load), ${SECS}s"
timeout -s INT "$SECS" "$BIN" -d "$DATA" -D "$DEVICE" -s "$SYNTH" -p "$MID" -l -S 10 2>&1 | grep -E 'periods=|SCHED|normal scheduling|ALSA'

echo "== with stress-ng $STRESS, ${SECS}s"
# shellcheck disable=SC2086
stress-ng $STRESS --timeout "$((SECS + 5))s" -q &
STRESS_PID=$!
sleep 2
timeout -s INT "$SECS" "$BIN" -d "$DATA" -D "$DEVICE" -s "$SYNTH" -p "$MID" -l -S 10 2>&1 | grep -E 'periods=|SCHED|normal scheduling|ALSA'
