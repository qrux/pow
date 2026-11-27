#!/bin/bash
# headless-smoke.sh: run the installed pow --headless for about 6 seconds on this Mac, stop it with SIGTERM, and
# show what it logged; then confirm that no powermetrics is left behind. Mac only (real sudo powermetrics).
# November 17th, 2026. Asks for your password once (sudo -v), as pow --headless never prompts.
set -o pipefail
CSV="$HOME/jorp/pow/logs/headless-smoke.csv"
sudo -v || { echo "headless-smoke: sudo refused"; exit 1; }
"$HOME/bin/pow" --headless --log "$CSV" &
p=$!
sleep 6
kill -TERM "$p"
wait "$p"; echo "pow exit: $?"
echo "--- $CSV (header and first rows; cpu_mw, gpu_mw and ane_mw must have numbers)"
head -4 "$CSV" | cut -c1-110
if pgrep -x powermetrics >/dev/null; then echo "headless-smoke: FAIL: a powermetrics is still running (unless another pow or powermetrics was already running)"; exit 1; fi
echo "headless-smoke: ok, no powermetrics left running"
