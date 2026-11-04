#!/usr/bin/env bash
# End-to-end run of the whole chain on test recordings, with the real AI library.
#   scripts/e2e.sh <wvproc> <ai-library> <clean speech wav> <outdir>
# Writes processed audio + metrics.md to outdir.
set -euo pipefail
WV="$1"; AI="$2"; CLEAN="$3"; OUT="$4"
PY="${PYTHON:-python}"
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$OUT"
"$PY" "$HERE/make_noise.py" "$OUT"
{
  echo "| scenario | AI | latency | CPU (1 core) | speech level | pauses | colour error | high band | voice profile |"
  echo "|---|---|---|---|---|---|---|---|---|"
} > "$OUT/metrics.md"
for nz in fan keyboard fan_keyboard; do
  mkdir -p "$OUT/$nz"
  "$PY" "$HERE/make_test_audio.py" "$CLEAN" "$OUT/noise_$nz.wav" "$OUT/$nz"
  for mic in headset usbmic; do
    extra=""; input="$OUT/$nz/$mic.wav"
    if [ "$mic" = headset ]; then extra="--bandlimit 16000 --save-input $OUT/$nz/${mic}_in.wav"; input="$OUT/$nz/${mic}_in.wav"; fi
    log=$("$WV" "$OUT/$nz/$mic.wav" "$OUT/$nz/${mic}_out.wav" --ai "$AI" $extra)
    echo "$log" > "$OUT/$nz/${mic}_log.txt"
    frames=$(echo "$log" | grep 'learning pass' | sed -E 's/.*: ([0-9]+) speech.*/\1/')
    if [ "${frames:-0}" -lt 300 ]; then echo "voice profile learned only ${frames:-0} frames on $mic + $nz" >&2; exit 1; fi
    m=$("$PY" "$HERE/evaluate.py" "$OUT/$nz/clean_ref.wav" "$input" "$OUT/$nz/${mic}_out.wav")
    echo "$m" > "$OUT/$nz/${mic}_metrics.txt"
    ai=$(echo "$log" | grep '^AI:' | cut -d' ' -f2)
    lat=$(echo "$log" | grep '^latency' | cut -d' ' -f2)
    cpu=$(echo "$log" | grep 'total' | sed -E 's/.*total ([0-9.]+) %.*/\1 %/')
    lvl=$(echo "$m" | grep 'speech level' | sed -E 's/.*in +(-?[0-9.]+) dBFS +out +(-?[0-9.]+).*/\1 → \2/')
    pz=$(echo "$m" | grep 'mid-file pauses' | sed -E 's/.*in +(-?[0-9.]+) dBFS +out +(-?[0-9.]+).*/\1 → \2/')
    col=$(echo "$m" | grep 'colour error' | sed -E 's/.*in ([0-9.]+) dB +-> +out ([0-9.]+).*/\1 → \2/')
    hb=$(echo "$m" | grep 'high band' | sed -E 's/.*in +(-?[0-9.]+) +out +(-?[0-9.]+).*/\1 → \2/')
    echo "| $mic + $nz | $ai | $lat ms | $cpu | $lvl | $pz | $col | $hb | $frames frames |" >> "$OUT/metrics.md"
  done
done
cat "$OUT/metrics.md"
