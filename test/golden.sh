#!/bin/bash
# Make test/golden from the pow built now. Run it only when a change to what pow draws or writes is meant, after
# looking at the change: make check then holds every later build to it. Run from the pow folder: make && bash test/golden.sh
cd "$(dirname "$0")/.." || exit 1
[ -x ./pow ] || { echo "golden: build pow first (make)"; exit 1; }
for seed in 7 42 1234; do [ -f "test/fixtures/data-$seed.txt" ] || python3 test/mkdata.py $seed > "test/fixtures/data-$seed.txt"; done
mkdir -p test/golden
bash test/produce.sh test/golden && echo "golden: wrote $(ls test/golden | wc -l | tr -d ' ') files in test/golden"
