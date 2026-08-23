#!/bin/bash
# Fail if a sketch directory holds its own COPY of a shared/ file.
#
# Why this exists: Arduino only compiles .cpp files that live in the sketch
# directory, so every shared source needs a same-named file there. The safe form
# is a one-line stub that #includes ../shared/<file>. A full copy looks identical
# and works — until shared/ is fixed and the copy is not.
#
# This bit hard on 2026-08-16. A UB fix went into shared/UartOtaProtocol.cpp; the
# Sense picked it up (stub) and the LCD did not (copy), so a valid 512-byte chunk
# kept parsing as data_len=59649 AFTER the fix was written, built and flashed.
# The unchanged failure value was the only clue. Separately, both boards carried
# a stale UartOtaProtocol.h using the SAME include guard as the shared header --
# two headers, one guard, so whichever landed first silently suppressed the other
# and the MSG_IMG_* constants disappeared with no error where the mistake was.
#
# Generated per-board files are legitimately different and are exempt below.
set -u

cd "$(dirname "$0")/../halo_ota_demo/firmware" || exit 2

# Regenerated per board (different BOARD:, build timestamp) -- copies are correct.
is_exempt() {
  case "$1" in
    BuildInfo.cpp|BuildInfo.h|Version.h|MqttSecrets.local.cpp|MqttSecrets.local.h) return 0 ;;
    *) return 1 ;;
  esac
}

fail=0
for d in halo_lcd_prod halo_sense_prod; do
  [ -d "$d" ] || continue
  for f in "$d"/*.c "$d"/*.cpp "$d"/*.h; do
    [ -f "$f" ] || continue
    b=$(basename "$f")
    [ -f "shared/$b" ] || continue
    is_exempt "$b" && continue
    # A stub is short and includes the shared file. Anything else is a copy.
    if grep -q '#include "\.\./shared/' "$f" && [ "$(wc -l < "$f")" -lt 25 ]; then
      continue
    fi
    if diff -q "$f" "shared/$b" >/dev/null 2>&1; then
      echo "COPY (identical, will drift): $f"
    else
      echo "COPY + ALREADY DRIFTED:       $f  [$(diff "$f" "shared/$b" | grep -c '^[<>]') differing lines]"
    fi
    fail=1
  done
done

if [ "$fail" -ne 0 ]; then
  echo
  echo "Replace each with a stub:"
  echo '    // Stub-include: single source of truth is ../shared/<file>'
  echo '    #include "../shared/<file>"'
  exit 1
fi

echo "OK: no duplicate copies of shared/ files in any sketch directory."
