#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../producer" && pwd)"
. "${SCRIPT_DIR}/build_env.sh"

SOCKET="${VIVID_DISPLAY_SOCKET:-${VIVID_DEFAULT_DISPLAY_SOCKET}}"
CONFIG="${VIVID_PRODUCER_CONFIG:-${VIVID_DIRECT_RUN_CONFIG_FILE}}"
PRODUCER_BIN="${VIVID_PRODUCER_BIN:-${VIVID_DIRECT_RUN_PRODUCER_BIN}}"
SCENE_LIBRARY="${VIVID_SCENE_LIBRARY:-${VIVID_DIRECT_RUN_SCENE_LIBRARY}}"
VIDEO_LIBRARY="${VIVID_VIDEO_LIBRARY:-${VIVID_DIRECT_RUN_VIDEO_LIBRARY}}"
WEB_LIBRARY="${VIVID_WEB_LIBRARY:-${VIVID_DIRECT_RUN_WEB_LIBRARY}}"
WEB_CEF_DIR="${VIVID_WEB_CEF_DIR:-${VIVID_DIRECT_RUN_WEB_OUT_DIR}}"
GBM_SHIM_LIBRARY="${VIVID_GBM_USAGE_SHIM:-${VIVID_DIRECT_RUN_GBM_USAGE_SHIM}}"

if [[ ! -x "${PRODUCER_BIN}" ]]; then
  echo "producer binary not found: ${PRODUCER_BIN}" >&2
  echo "run tools/vivid.sh direct-run build first" >&2
  exit 1
fi

if [[ ! -f "${SCENE_LIBRARY}" ]]; then
  echo "scene library not found: ${SCENE_LIBRARY}" >&2
  echo "run tools/vivid.sh direct-run build first" >&2
  exit 1
fi

if [[ ! -f "${VIDEO_LIBRARY}" ]]; then
  echo "video library not found: ${VIDEO_LIBRARY}" >&2
  echo "run tools/vivid.sh direct-run build first" >&2
  exit 1
fi

if [[ ! -f "${WEB_LIBRARY}" ]]; then
  echo "web library not found: ${WEB_LIBRARY}" >&2
  echo "run tools/vivid.sh direct-run build first" >&2
  exit 1
fi

mkdir -p "$(dirname "${SOCKET}")"

export VIVID_DISPLAY_SOCKET="${SOCKET}"
export VIVID_SCENE_LIBRARY="${SCENE_LIBRARY}"
export VIVID_VIDEO_LIBRARY="${VIDEO_LIBRARY}"
export VIVID_WEB_LIBRARY="${WEB_LIBRARY}"
export VIVID_WEB_CEF_DIR="${WEB_CEF_DIR}"
export LD_LIBRARY_PATH="$(dirname "${SCENE_LIBRARY}"):$(dirname "${VIDEO_LIBRARY}"):$(dirname "${WEB_LIBRARY}"):${LD_LIBRARY_PATH:-}"
export G_MESSAGES_DEBUG="${G_MESSAGES_DEBUG:-all}"
export GST_DEBUG="${GST_DEBUG:-2}"

gbm_shim_rewrite="${VIVID_GBM_SHIM_REWRITE:-0}"
gbm_shim_auto="${VIVID_GBM_SHIM_AUTO:-1}"
gbm_shim_preload=0
if [[ "${gbm_shim_rewrite}" != "0" ]]; then
  gbm_shim_preload=1
elif [[ "${gbm_shim_auto}" != "0" && -f "${GBM_SHIM_LIBRARY}" ]]; then
  gbm_shim_preload=1
fi

if [[ "${gbm_shim_preload}" != "0" ]]; then
  if [[ ! -f "${GBM_SHIM_LIBRARY}" ]]; then
    echo "GBM usage shim not found: ${GBM_SHIM_LIBRARY}" >&2
    echo "run tools/vivid.sh direct-run build first" >&2
    exit 1
  fi

  # The shim must be preloaded before CEF forks its GPU process. The web
  # backend decides later, after resolving render-device, whether the shim
  # should only pass through or redirect Chromium's GBM allocator to NVIDIA.
  case ":${LD_PRELOAD:-}:" in
    *":${GBM_SHIM_LIBRARY}:"*) ;;
    *) export LD_PRELOAD="${GBM_SHIM_LIBRARY}${LD_PRELOAD:+:${LD_PRELOAD}}" ;;
  esac
  if [[ "${gbm_shim_auto}" != "0" && "${gbm_shim_rewrite}" = "0" ]]; then
    export VIVID_GBM_SHIM_LOG="${VIVID_GBM_SHIM_LOG:-0}"
  else
    export VIVID_GBM_SHIM_LOG="${VIVID_GBM_SHIM_LOG:-1}"
  fi
fi

if [[ "${gbm_shim_preload}" != "0" ]]; then
  echo "Vivid Wallpaper direct-run GBM shim: ${GBM_SHIM_LIBRARY} auto=${gbm_shim_auto} rewrite=${gbm_shim_rewrite} log=${VIVID_GBM_SHIM_LOG}"
fi
exec "${PRODUCER_BIN}" --socket "${SOCKET}" --config "${CONFIG}" "$@"
