#!/usr/bin/env bash
# Build and run the SmartCap FSM + radio-adapter host tests against a mock
# radio backend. No ESP-IDF / real Wi-Fi required.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="/tmp/opencode/smartcap_fsm_test"

g++ -std=gnu++17 -Wall -Wextra -I "$ROOT/include" \
    "$ROOT/src/smartcap_score.cpp" \
    "$ROOT/src/smartcap_table.cpp" \
    "$ROOT/src/smartcap_focus.cpp" \
    "$ROOT/src/smartcap_attack.cpp" \
    "$ROOT/src/smartcap_radio.cpp" \
    "$ROOT/src/smartcap_fsm.cpp" \
    "$ROOT/src/smartcap_result.cpp" \
    "$ROOT/tools/smartcap_fsm_test.cpp" \
    -o "$OUT"

"$OUT"