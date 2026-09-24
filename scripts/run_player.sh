#!/bin/sh
# Run komi-player on the PSP through PSPLink and wait for its result log.
#   scripts/run_player.sh           build, load host0:/komi-player.prx, wait
# The PSP must be on USB with PSPLink open. Results: field-logs/player-<time>/.
set -eu
PROJECT_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT="$PROJECT_ROOT/vendor/tilefinch/build-preset-psp/komi-player"
LIMIT_SECONDS=${LIMIT_SECONDS:-1500}
"$PROJECT_ROOT/scripts/build_player.sh" > /dev/null
export HOST_ROOT="$OUT"
"$PROJECT_ROOT/scripts/psplink.sh" ready
# A previous run that is still playing holds the memory the new one needs.
waited=0
while "$PROJECT_ROOT/scripts/psplink.sh" exec 'modlist' 2>/dev/null | grep -q komi_player; do
    [ "$waited" -ge 900 ] && { echo "a previous komi-player is still running" >&2; exit 1; }
    [ "$waited" -eq 0 ] && echo "waiting for the previous komi-player to finish..."
    sleep 10; waited=$((waited + 10))
done
rm -f "$OUT/komi-player.txt"
"$PROJECT_ROOT/scripts/psplink.sh" exec 'ld host0:/komi-player.prx'
started=$(date +%s)
while :; do
    if [ -f "$OUT/komi-player.txt" ] && grep -q -E '^(end|WATCHDOG)' "$OUT/komi-player.txt"; then
        break
    fi
    if [ $(( $(date +%s) - started )) -ge "$LIMIT_SECONDS" ]; then
        echo "no end line after ${LIMIT_SECONDS}s" >&2
        break
    fi
    sleep 5
done
dest="$PROJECT_ROOT/field-logs/player-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$dest"
for f in komi-player.txt tilefinch-validation.txt tilefinch-crash.txt; do
    [ -f "$OUT/$f" ] && cp "$OUT/$f" "$dest/"
done
echo "logs: $dest"
cat "$dest/komi-player.txt" 2>/dev/null || true
