#!/usr/bin/env bash
# Runs all offline unit tests (scripts/run_*_tests.sh and scripts/*_test.py) in
# parallel. Needs only a C++17 compiler and python3, no SDK and no game server.
# Prints one line per failing test and a summary; exit status 1 if any failed.
# Usage: scripts/run_unit_tests.sh   (UNIT_JOBS=N limits parallel jobs, default nproc)
cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 1
TMP="$(mktemp -d)" || exit 1  # kept on failure for the full logs

tests=()
for f in scripts/run_*_tests.sh scripts/*_test.py; do
  [[ -f "$f" && "$f" != scripts/run_unit_tests.sh ]] && tests+=("$f")
done

run_one() {
  local f="$1" log="$TMP/$(basename "$1").log" cmd=(bash "$1")
  [[ "$f" == *.py ]] && cmd=(python3 "$f")
  if "${cmd[@]}" >"$log" 2>&1; then echo "PASS $f"; else echo "FAIL $f ($log)"; tail -20 "$log"; fi
}
export -f run_one
export TMP

printf '%s\0' "${tests[@]}" | xargs -0 -n1 -P "${UNIT_JOBS:-$(nproc)}" bash -c 'run_one "$0"' >"$TMP/result.txt"
failed="$(grep -c '^FAIL ' "$TMP/result.txt")"
if (( failed )); then
  grep -v '^PASS ' "$TMP/result.txt"
  echo "Unit tests: $failed of ${#tests[@]} FAILED"
  exit 1
fi
rm -rf "$TMP"
echo "Unit tests: ${#tests[@]}/${#tests[@]} PASS"
