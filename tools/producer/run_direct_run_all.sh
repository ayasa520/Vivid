#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRODUCER_PID=""
WEBUI_PID=""
CLEANED_UP=0

cleanup() {
  if [[ "${CLEANED_UP}" -ne 0 ]]; then
    return
  fi
  CLEANED_UP=1

  if [[ -n "${PRODUCER_PID}" ]]; then
    kill "${PRODUCER_PID}" 2>/dev/null || true
  fi
  if [[ -n "${WEBUI_PID}" ]]; then
    kill "${WEBUI_PID}" 2>/dev/null || true
  fi

  if [[ -n "${PRODUCER_PID}" ]]; then
    wait "${PRODUCER_PID}" 2>/dev/null || true
  fi
  if [[ -n "${WEBUI_PID}" ]]; then
    wait "${WEBUI_PID}" 2>/dev/null || true
  fi
}

trap cleanup EXIT INT TERM

"${SCRIPT_DIR}/run_direct_run_producer.sh" &
PRODUCER_PID="$!"

"${SCRIPT_DIR}/run_direct_run_webui.sh" &
WEBUI_PID="$!"

set +e
wait -n "${PRODUCER_PID}" "${WEBUI_PID}"
STATUS="$?"
set -e

cleanup
exit "${STATUS}"
