#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../producer" && pwd)"
. "${SCRIPT_DIR}/build_env.sh"

SOCKET="${VIVID_DISPLAY_SOCKET:-${VIVID_DEFAULT_DISPLAY_SOCKET}}"
HOST="${VIVID_WEBUI_HOST:-127.0.0.1}"
PORT="${VIVID_WEBUI_PORT:-8765}"
PYTHON_BIN="${VIVID_PYTHON:-python3}"
WEBUI_SERVER="${VIVID_WEBUI_SERVER:-${VIVID_WEBUI_SOURCE_DIR}/vivid_webui_server.py}"
WEBUI_ROOT="${VIVID_WEBUI_ROOT:-${VIVID_WEBUI_SOURCE_DIR}}"

mkdir -p "$(dirname "${SOCKET}")"

export VIVID_DISPLAY_SOCKET="${SOCKET}"

exec "${PYTHON_BIN}" "${WEBUI_SERVER}" \
  --host "${HOST}" \
  --port "${PORT}" \
  --socket "${SOCKET}" \
  --web-root "${WEBUI_ROOT}" \
  "$@"
