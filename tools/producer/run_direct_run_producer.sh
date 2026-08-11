#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../producer" && pwd)"
. "${SCRIPT_DIR}/build_env.sh"

SOCKET="${VIVID_DISPLAY_SOCKET:-${VIVID_DEFAULT_DISPLAY_SOCKET}}"
CONFIG="${VIVID_PRODUCER_CONFIG:-${VIVID_DIRECT_RUN_CONFIG_FILE}}"
PRODUCER_BIN="${VIVID_PRODUCER_BIN:-${VIVID_DIRECT_RUN_PRODUCER_BIN}}"
RENDERER_ROOT="${VIVID_RENDERER_REGISTRY:-${VIVID_DIRECT_RUN_RENDERER_ROOT}}"

if [[ ! -x "${PRODUCER_BIN}" ]]; then
  echo "producer binary not found: ${PRODUCER_BIN}" >&2
  echo "run tools/vivid.sh direct-run build first" >&2
  exit 1
fi

for renderer_file in \
  "${RENDERER_ROOT}/scene/${VIVID_SCENE_EXECUTABLE_NAME}" \
  "${RENDERER_ROOT}/video/${VIVID_VIDEO_EXECUTABLE_NAME}" \
  "${RENDERER_ROOT}/web/${VIVID_WEB_EXECUTABLE_NAME}"; do
  if [[ ! -x "${renderer_file}" ]]; then
    echo "renderer executable not found: ${renderer_file}" >&2
    echo "run tools/vivid.sh direct-run build first" >&2
    exit 1
  fi
done

for manifest_file in \
  "${RENDERER_ROOT}/${VIVID_SCENE_MANIFEST_NAME}" \
  "${RENDERER_ROOT}/${VIVID_VIDEO_MANIFEST_NAME}" \
  "${RENDERER_ROOT}/${VIVID_WEB_MANIFEST_NAME}"; do
  if [[ ! -f "${manifest_file}" ]]; then
    echo "renderer manifest not found: ${manifest_file}" >&2
    echo "run tools/vivid.sh direct-run build first" >&2
    exit 1
  fi
done

mkdir -p "$(dirname "${SOCKET}")"

export VIVID_DISPLAY_SOCKET="${SOCKET}"
export G_MESSAGES_DEBUG="${G_MESSAGES_DEBUG:-all}"
export GST_DEBUG="${GST_DEBUG:-2}"
exec "${PRODUCER_BIN}" \
  --socket "${SOCKET}" \
  --config "${CONFIG}" \
  --renderer-registry "${RENDERER_ROOT}" \
  "$@"
