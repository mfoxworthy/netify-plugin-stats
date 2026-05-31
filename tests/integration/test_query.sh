#!/bin/sh
set -e
DIR="$(mktemp -d)"
"$SEED" "$DIR"
OUT="$("$QUERY" "$DIR")"
echo "$OUT"
echo "$OUT" | grep -q '"netflix"' || { echo "FAIL: netflix missing"; rm -rf "$DIR"; exit 1; }
echo "$OUT" | grep -q '5000'       || { echo "FAIL: rx_bytes 5000 missing"; rm -rf "$DIR"; exit 1; }
echo "$OUT" | grep -q '"youtube"'  || { echo "FAIL: youtube missing"; rm -rf "$DIR"; exit 1; }
echo "PASS"
rm -rf "$DIR"
