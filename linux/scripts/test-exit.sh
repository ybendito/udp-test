#!/usr/bin/env bash
# Quick exit tests for boostudp_server (run on f2 or Linux).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SERVER="${ROOT}/linux/build/boostudp_server"
SENDER="${SCRIPT_DIR}/send_batches.py"
PORT="${PORT:-19000}"
BIND="127.0.0.1"
LOG="/tmp/boostudp-test-$$.log"

if [[ ! -x "${SERVER}" ]]; then
  echo "Build server first: ${ROOT}/linux/scripts/build.sh" >&2
  exit 1
fi

run_case() {
  local name="$1"
  shift
  echo "=== ${name} ==="
  rm -f "${LOG}"
  "$@" >"${LOG}" 2>&1 &
  local pid=$!
  sleep 0.3
  python3 "${SENDER}" --dest "${BIND}" --port "${PORT}" --size 4000 --count "${SEND_COUNT:-1}"
  if wait "${pid}"; then
    echo "server exit: 0"
  else
    echo "server exit: $?"
  fi
  grep -E '^(listen|ok |batches=)' "${LOG}" || true
  echo
}

export PORT BIND

SEND_COUNT=1
run_case "server --count 1, client 1 batch" \
  "${SERVER}" --bind "${BIND}" --port "${PORT}" -v --count 1

SEND_COUNT=3
run_case "server --count 3, client 3 batches" \
  "${SERVER}" --bind "${BIND}" --port "${PORT}" -v --count 3

SEND_COUNT=1
run_case "server idle (no --count), client 1 batch" \
  timeout -s TERM 8 "${SERVER}" --bind "${BIND}" --port "${PORT}" -v

echo "done"
