#!/bin/bash
# Write everything make check compares into folder $1: replays, drawn frames, a live run with --log, and pow -h.
# test/golden.sh writes them into test/golden; test/check.sh writes them into a temporary folder and compares.
# Run from the pow folder, after make. Needs python3 (the stand-in sudo plays back recorded powermetrics with it).
D=$1; mkdir -p "$D" || exit 1
if [ -z "$LIBS" ]; then
  if [ "$(uname)" = Darwin ]; then LIBS="-framework CoreFoundation -framework IOKit -lz"; else LIBS="-lz -lm -lpthread"; fi
fi
export POW_TEST_HOST=1
for f in test/fixtures/sample.csv test/fixtures/powermetrics.txt; do
  for sz in "80 24" "100 30" "120 40" "159 60" "160 48" "189 54" "250 70"; do
    set -- $sz
    COLUMNS=$1 LINES=$2 ./pow --replay "$f" > "$D/replay-$(basename "$f")-$1x$2.txt" 2>&1
  done
done
${CC:-cc} -std=gnu11 -O2 -w -o "$D/c_frames" test/c_frames.c src/util.c src/data.c src/view.c src/term.c src/mac.c $LIBS || exit 1
for seed in 7 42 1234; do
  for m in txt px; do "$D/c_frames" "test/fixtures/data-$seed.txt" $m > "$D/frames-$seed-$m.bin"; done
done
chmod +x test/fakebin/sudo
PATH="$PWD/test/fakebin:$PATH" POW_FAKE_PM="$PWD/test/fixtures/powermetrics.txt" COLUMNS=189 LINES=54 ./pow --sudo --no-smc --no-media --no-temps --log "$D/live-log.csv" > "$D/live-raw.bin" 2>/dev/null
python3 - "$D" <<'PY'
import sys
d = sys.argv[1]
a = open(f'{d}/live-raw.bin', 'rb').read()
a = a[a.find(b'\x1b[?1049h'):]                       # the first line names the log file: start at the alternate screen
open(f'{d}/live.bin', 'wb').write(b'\n=====\n'.join(f.split(b'\x1b[?2026l')[0] for f in a.split(b'\x1b[?2026h')[1:]))
open(f'{d}/live.csv', 'w').writelines(l.split(',', 1)[1] for l in open(f'{d}/live-log.csv'))   # without the time column
PY
rm -f "$D/live-raw.bin" "$D/live-log.csv" "$D/live-log.csv.1" "$D/c_frames"
./pow -h > "$D/help.txt"
