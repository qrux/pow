#!/bin/bash
# Compare what this build of pow draws and writes with test/golden (made by test/golden.sh): every difference is a
# failure. Replays at seven terminal sizes; fifteen drawn frames from three made-up states, as text and as LED images
# (decoded pixels compared); images by shared memory and the skipping of unchanged charts; a live run on recorded
# powermetrics with --log; pow -h. Needs python3. Run from the pow folder: make check
cd "$(dirname "$0")/.." || exit 1
[ -d test/golden ] || { echo "check: no test/golden yet; make it with: bash test/golden.sh"; exit 1; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
bash test/produce.sh "$T/now" || { echo "check: could not produce the outputs"; exit 1; }
G=test/golden N=$T/now fail=0
note() { if [ "$1" = 0 ]; then echo "  same  $2"; else echo "  DIFF  $2"; fail=1; fi; }
echo "replays"
for f in "$G"/replay-*.txt; do b=$(basename "$f"); cmp -s "$f" "$N/$b"; note $? "${b#replay-}"; done
echo "drawn frames"
for seed in 7 42 1234; do
  for m in txt px; do
    python3 test/compare.py "$G/frames-$seed-$m.bin" "$N/frames-$seed-$m.bin" > "$T/cmp.txt"; r=$?
    note $r "seed $seed, $( [ $m = px ] && echo LED images || echo text ), 15 frames"
    [ $r = 0 ] || sed 's/^/        /' "$T/cmp.txt"
  done
done
echo "images by shared memory, and skipping unchanged charts (the live view)"
${CC:-cc} -std=gnu11 -O2 -w -o "$T/c_frames" test/c_frames.c src/util.c src/data.c src/view.c src/term.c src/mac.c $LIBS || exit 1
"$T/c_frames" test/fixtures/data-7.txt shm > "$T/shm.bin"
python3 test/compare.py "$G/frames-7-px.bin" "$T/shm.bin" > "$T/cmp.txt"; r=$?
note $r "shared-memory pixels match the golden ones, 15 frames"; [ $r = 0 ] || sed 's/^/        /' "$T/cmp.txt"
"$T/c_frames" test/fixtures/data-7.txt cache > "$T/cache.bin"
python3 test/compare.py "$N/frames-7-px.bin" "$T/cache.bin" cache > "$T/cmp.txt"; r=$?
note $r "skipped charts were unchanged, sent ones match, gone ones deleted"; [ $r = 0 ] || sed 's/^/        /' "$T/cmp.txt"
echo "live run on recorded powermetrics, with --log"
cmp -s "$G/live.bin" "$N/live.bin"; note $? "$(python3 -c "import sys; print(open(sys.argv[1],'rb').read().count(b'\n=====\n') + 1)" "$N/live.bin") frames"
cmp -s "$G/live.csv" "$N/live.csv"; note $? "the --log CSV, $(wc -l < "$N/live.csv" | tr -d ' ') lines, apart from the time column"
echo "help"
cmp -s "$G/help.txt" "$N/help.txt"; note $? "pow -h"
[ $fail = 0 ] && echo "check: everything matches test/golden" || echo "check: DIFFERENCES above (if the change is meant: look at it, then bash test/golden.sh)"
exit $fail
