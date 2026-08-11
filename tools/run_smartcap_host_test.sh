#!/usr/bin/env bash
# Build and run the SmartCap host test - the scoring/table/focus/attack logic
# compiled with plain g++, no ESP-IDF / Wi-Fi stack required.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="/tmp/opencode/smartcap_host_test"

g++ -std=gnu++17 -Wall -Wextra -I "$ROOT/include" \
    "$ROOT/src/smartcap_score.cpp" \
    "$ROOT/src/smartcap_table.cpp" \
    "$ROOT/src/smartcap_focus.cpp" \
    "$ROOT/src/smartcap_attack.cpp" \
    "$ROOT/tools/smartcap_host_test.cpp" \
    -o "$OUT"

"$OUT"