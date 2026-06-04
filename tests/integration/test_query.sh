#!/bin/sh
set -e
DIR="$(mktemp -d)"
trap 'rm -rf "$DIR"' EXIT

# Seed br-lan and wan interface subdirs
"$SEED" "$DIR"

# ── per-interface: br-lan ─────────────────────────────────────────────────────
OUT_LAN="$("$QUERY" "$DIR" br-lan)"
echo "$OUT_LAN" | grep -q '"netflix"'  || { echo "FAIL: netflix missing from br-lan"; exit 1; }
echo "$OUT_LAN" | grep -q '"youtube"'  || { echo "FAIL: youtube missing from br-lan"; exit 1; }
echo "$OUT_LAN" | grep -q '"zoom"'    && { echo "FAIL: zoom leaked into br-lan"; exit 1; }
echo "PASS: br-lan single-interface query"

# ── per-interface: wan ────────────────────────────────────────────────────────
OUT_WAN="$("$QUERY" "$DIR" wan)"
echo "$OUT_WAN" | grep -q '"zoom"'     || { echo "FAIL: zoom missing from wan"; exit 1; }
echo "$OUT_WAN" | grep -q '"youtube"'  || { echo "FAIL: youtube missing from wan"; exit 1; }
echo "$OUT_WAN" | grep -q '"netflix"' && { echo "FAIL: netflix leaked into wan"; exit 1; }
echo "PASS: wan single-interface query"

# ── combined merge (optional — requires QUERY_MERGED binary) ──────────────────
if [ -n "$QUERY_MERGED" ]; then
    OUT_ALL="$("$QUERY_MERGED" "$DIR")"
    echo "$OUT_ALL" | grep -q '"netflix"' || { echo "FAIL: netflix missing from combined"; exit 1; }
    echo "$OUT_ALL" | grep -q '"zoom"'    || { echo "FAIL: zoom missing from combined"; exit 1; }
    echo "$OUT_ALL" | grep -q '"youtube"' || { echo "FAIL: youtube missing from combined"; exit 1; }
    # youtube appears in both stores; combined rx_bytes should be 3000+1000=4000
    echo "$OUT_ALL" | grep -q '4000'      || { echo "FAIL: youtube combined rx_bytes should be 4000"; exit 1; }
    echo "PASS: combined merge query"
fi

# ── list_interfaces (optional — requires LIST_IFACES binary) ──────────────────
if [ -n "$LIST_IFACES" ]; then
    OUT_LIST="$("$LIST_IFACES" "$DIR")"
    echo "$OUT_LIST" | grep -q '"br-lan"' || { echo "FAIL: br-lan missing from list_interfaces"; exit 1; }
    echo "$OUT_LIST" | grep -q '"wan"'    || { echo "FAIL: wan missing from list_interfaces"; exit 1; }
    echo "PASS: list_interfaces"
fi

echo "PASS: all integration checks"
